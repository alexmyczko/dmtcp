/****************************************************************************
 *   Copyright (C) 2006-2013 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *  This file is part of DMTCP.                                             *
 *                                                                          *
 *  DMTCP is free software: you can redistribute it and/or                  *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP is distributed in the hope that it will be useful,                *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "jassert.h"
#include "syscallwrappers.h"
#include "threadsync.h"
#include "workerstate.h"

using namespace dmtcp;

/*
 * _wrapperExecutionLock is used to make the checkpoint safe by making sure
 *   that no user-thread is executing any DMTCP wrapper code when it receives
 *   the checkpoint signal.
 * Working:
 *   On entering the wrapper in DMTCP, the user-thread acquires the read lock,
 *     and releases it before leaving the wrapper.
 *   When the Checkpoint-thread wants to send the SUSPEND signal to user
 *     threads, it must acquire the write lock. It is blocked until all the
 *     existing read-locks by user threads have been released. NOTE that this
 *     is a WRITER-PREFERRED lock.
 *
 * There is a corner case too -- the newly created thread that has not been
 *   initialized yet; we need to take some extra efforts for that.
 * Here are the steps to handle the newly created uninitialized thread:
 *   A counter (_uninitializedThreadCount) for the number of newly
 *     created uninitialized threads is kept.  The counter is made
 *     thread-safe by using a mutex.
 *   The calling thread (parent) increments the counter before calling clone.
 *   The newly created child thread decrements the counter at the end of
 *     initialization in MTCP/DMTCP.
 *   After acquiring the Write lock, the checkpoint thread waits until the
 *     number of uninitialized threads is zero. At that point, no thread is
 *     executing in the clone wrapper and it is safe to do a checkpoint.
 *
 * XXX: Currently this security is provided only for the clone wrapper; this
 * should be extended to other calls as well.           -- KAPIL
 */

// NOTE: PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP is not POSIX.
static pthread_rwlock_t
  _wrapperExecutionLock = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
static pthread_rwlock_t
  _threadCreationLock = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
static bool _wrapperExecutionLockAcquiredByCkptThread = false;
static bool _threadCreationLockAcquiredByCkptThread = false;

static pthread_mutex_t theCkptCanStart = PTHREAD_MUTEX_INITIALIZER;
static int ckptCanStartCount = 0;

static pthread_mutex_t libdlLock = PTHREAD_MUTEX_INITIALIZER;
static pid_t libdlLockOwner = 0;

static pthread_mutex_t uninitializedThreadCountLock = PTHREAD_MUTEX_INITIALIZER;
static int _uninitializedThreadCount = 0;
static bool _checkpointThreadInitialized = false;

static pthread_mutex_t preResumeThreadCountLock = PTHREAD_MUTEX_INITIALIZER;

static __thread int _wrapperExecutionLockLockCount = 0;
static __thread int _threadCreationLockLockCount = 0;
#if TRACK_DLOPEN_DLSYM_FOR_LOCKS
static __thread bool _threadPerformingDlopenDlsym = false;
#endif // if TRACK_DLOPEN_DLSYM_FOR_LOCKS
static __thread bool _isOkToGrabWrapperExecutionLock = true;
static __thread bool _hasThreadFinishedInitialization = false;


/* The following two functions dmtcp_libdlLock{Lock,Unlock} are used by dlopen
 * plugin.
 */
extern "C" int
dmtcp_libdlLockLock()
{
  return ThreadSync::libdlLockLock();
}

extern "C" void
dmtcp_libdlLockUnlock()
{
  ThreadSync::libdlLockUnlock();
}

void
ThreadSync::initThread()
{
  // We initialize these thread-local variables here. If not done here,
  // there can be a race between checkpoint processing and this
  // thread trying to initialize some thread-local variable.
  // Here is a possible calltrace:
  // pthread_start -> threadFinishedInitialization -> stopthisthread ->
  // callbackHoldsAnyLocks -> JASSERT().
  _wrapperExecutionLockLockCount = 0;
  _threadCreationLockLockCount = 0;
#if TRACK_DLOPEN_DLSYM_FOR_LOCKS
  _threadPerformingDlopenDlsym = false;
#endif // if TRACK_DLOPEN_DLSYM_FOR_LOCKS
  _isOkToGrabWrapperExecutionLock = true;
  _hasThreadFinishedInitialization = false;
}

void
ThreadSync::initMotherOfAll()
{
  initThread();
  _hasThreadFinishedInitialization = true;
}

void
ThreadSync::acquireLocks()
{
  JASSERT(WorkerState::currentState() == WorkerState::PRESUSPEND);

  /* TODO: We should introduce the notion of lock ranks/priorities for all
   * these locks to prevent future deadlocks due to rank violation.
   */

  JTRACE("waiting for dmtcp_lock():"
         " to get synchronized with _runCoordinatorCmd if we use DMTCP API");
  _dmtcp_lock();

  JTRACE("Waiting for lock(&theCkptCanStart)");
  JASSERT(_real_pthread_mutex_lock(&theCkptCanStart) == 0)(JASSERT_ERRNO);

  JTRACE("Waiting for libdlLock");
  JASSERT(_real_pthread_mutex_lock(&libdlLock) == 0) (JASSERT_ERRNO);

  JTRACE("Waiting for threads creation lock");
  JASSERT(_real_pthread_rwlock_wrlock(&_threadCreationLock) == 0)
    (JASSERT_ERRNO);
  _threadCreationLockAcquiredByCkptThread = true;

  JTRACE("Waiting for other threads to exit DMTCP-Wrappers");
  JASSERT(_real_pthread_rwlock_wrlock(&_wrapperExecutionLock) == 0)
    (JASSERT_ERRNO);
  _wrapperExecutionLockAcquiredByCkptThread = true;

  JTRACE("Waiting for newly created threads to finish initialization")
    (_uninitializedThreadCount);
  waitForThreadsToFinishInitialization();

  unsetOkToGrabLock();
  JTRACE("Done acquiring all locks");
}

void
ThreadSync::releaseLocks()
{
  JASSERT(WorkerState::currentState() == WorkerState::SUSPENDED);

  JTRACE("Releasing ThreadSync locks");
  JASSERT(_real_pthread_rwlock_unlock(&_wrapperExecutionLock) == 0)
    (JASSERT_ERRNO);
  _wrapperExecutionLockAcquiredByCkptThread = false;
  JASSERT(_real_pthread_rwlock_unlock(&_threadCreationLock) == 0)
    (JASSERT_ERRNO);
  _threadCreationLockAcquiredByCkptThread = false;
  JASSERT(_real_pthread_mutex_unlock(&libdlLock) == 0) (JASSERT_ERRNO);
  JASSERT(_real_pthread_mutex_unlock(&theCkptCanStart) == 0) (JASSERT_ERRNO);

  _dmtcp_unlock();
  setOkToGrabLock();
}

void
ThreadSync::resetLocks()
{
  pthread_rwlock_t newLock = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;

  _wrapperExecutionLock = newLock;
  _threadCreationLock = newLock;

  _wrapperExecutionLockLockCount = 0;
  _threadCreationLockLockCount = 0;
#if TRACK_DLOPEN_DLSYM_FOR_LOCKS
  _threadPerformingDlopenDlsym = false;
#endif // if TRACK_DLOPEN_DLSYM_FOR_LOCKS
  _isOkToGrabWrapperExecutionLock = true;
  _hasThreadFinishedInitialization = true;

  pthread_mutex_t newCountLock = PTHREAD_MUTEX_INITIALIZER;
  uninitializedThreadCountLock = newCountLock;
  pthread_mutex_t newPreResumeThreadCountLock = PTHREAD_MUTEX_INITIALIZER;
  preResumeThreadCountLock = newPreResumeThreadCountLock;

  pthread_mutex_t newLibdlLock = PTHREAD_MUTEX_INITIALIZER;
  libdlLock = newLibdlLock;
  libdlLockOwner = 0;

  _checkpointThreadInitialized = false;
  _wrapperExecutionLockAcquiredByCkptThread = false;
  _threadCreationLockAcquiredByCkptThread = false;
}

bool
ThreadSync::isOkToGrabLock()
{
  return _isOkToGrabWrapperExecutionLock;
}

void
ThreadSync::setOkToGrabLock()
{
  _isOkToGrabWrapperExecutionLock = true;
}

void
ThreadSync::unsetOkToGrabLock()
{
  _isOkToGrabWrapperExecutionLock = false;
}

#if TRACK_DLOPEN_DLSYM_FOR_LOCKS
extern "C" LIB_PRIVATE
void
dmtcp_setThreadPerformingDlopenDlsym()
{
  ThreadSync::setThreadPerformingDlopenDlsym();
}

extern "C" LIB_PRIVATE
void
dmtcp_unsetThreadPerformingDlopenDlsym()
{
  ThreadSync::unsetThreadPerformingDlopenDlsym();
}

bool
ThreadSync::isThreadPerformingDlopenDlsym()
{
  return _threadPerformingDlopenDlsym;
}

void
ThreadSync::setThreadPerformingDlopenDlsym()
{
  _threadPerformingDlopenDlsym = true;
}

void
ThreadSync::unsetThreadPerformingDlopenDlsym()
{
  _threadPerformingDlopenDlsym = false;
}
#endif // if TRACK_DLOPEN_DLSYM_FOR_LOCKS

void
ThreadSync::delayCheckpointsLock()
{
  if (ckptCanStartCount++ == 0) {
    JASSERT(_real_pthread_mutex_lock(&theCkptCanStart) == 0)(JASSERT_ERRNO);
  }
}

void
ThreadSync::delayCheckpointsUnlock()
{
  if (--ckptCanStartCount == 0) {
    JASSERT(_real_pthread_mutex_unlock(&theCkptCanStart) == 0)(JASSERT_ERRNO);
  }
}

static void
incrementWrapperExecutionLockLockCount()
{
  _wrapperExecutionLockLockCount++;
}

static void
decrementWrapperExecutionLockLockCount()
{
  if (_wrapperExecutionLockLockCount <= 0) {
    JASSERT(false) (_wrapperExecutionLockLockCount)
    .Text("wrapper-execution lock count can't be negative");
  }
  _wrapperExecutionLockLockCount--;
}

static void
incrementThreadCreationLockLockCount()
{
  _threadCreationLockLockCount++;
}

static void
decrementThreadCreationLockLockCount()
{
  _threadCreationLockLockCount--;
}

bool
ThreadSync::libdlLockLock()
{
  int saved_errno = errno;
  bool lockAcquired = false;

  if ((WorkerState::currentState() == WorkerState::RUNNING ||
       WorkerState::currentState() == WorkerState::PRESUSPEND) &&
      libdlLockOwner != dmtcp_gettid()) {
    JASSERT(_real_pthread_mutex_lock(&libdlLock) == 0);
    libdlLockOwner = dmtcp_gettid();
    lockAcquired = true;
  }
  errno = saved_errno;
  return lockAcquired;
}

void
ThreadSync::libdlLockUnlock()
{
  int saved_errno = errno;

  JASSERT(libdlLockOwner == 0 || libdlLockOwner == dmtcp_gettid())
    (libdlLockOwner) (dmtcp_gettid());
  JASSERT(WorkerState::currentState() == WorkerState::RUNNING ||
          WorkerState::currentState() == WorkerState::PRESUSPEND);
  libdlLockOwner = 0;
  JASSERT(_real_pthread_mutex_unlock(&libdlLock) == 0);
  errno = saved_errno;
}

// XXX: Handle deadlock error code
// NOTE: Don't do any fancy stuff in this wrapper which can cause the process
// to go into DEADLOCK
bool
ThreadSync::wrapperExecutionLockLock()
{
  int saved_errno = errno;
  bool lockAcquired = false;

  while (1) {
    if ((WorkerState::currentState() == WorkerState::RUNNING ||
         WorkerState::currentState() == WorkerState::PRESUSPEND) &&
#if TRACK_DLOPEN_DLSYM_FOR_LOCKS
        isThreadPerformingDlopenDlsym() == false &&
#endif // if TRACK_DLOPEN_DLSYM_FOR_LOCKS
        isOkToGrabLock() == true &&
        _wrapperExecutionLockLockCount == 0) {
      incrementWrapperExecutionLockLockCount();
      int retVal = _real_pthread_rwlock_tryrdlock(&_wrapperExecutionLock);
      if (retVal != 0 && retVal == EBUSY) {
        decrementWrapperExecutionLockLockCount();
        struct timespec sleepTime = { 0, 100 * 1000 * 1000 };
        nanosleep(&sleepTime, NULL);
        continue;
      }
      if (retVal != 0 && retVal != EDEADLK) {
        fprintf(stderr, "ERROR %d at %s:%d %s: Failed to acquire lock\n",
                errno, __FILE__, __LINE__, __PRETTY_FUNCTION__);
        _exit(DMTCP_FAIL_RC);
      }

      // retVal should always be 0 (success) here.
      lockAcquired = retVal == 0 ? true : false;
      if (!lockAcquired) {
        decrementWrapperExecutionLockLockCount();
      }
    }
    break;
  }
  errno = saved_errno;
  return lockAcquired;
}

/*
 * Execute fork() and exec() wrappers in exclusive mode
 *
 * fork() and exec() wrappers pass on the state/information about the current
 * process/program to the to-be-created process/program.
 *
 * There can be a potential race in the wrappers if this information gets
 * changed between the point where it was acquired and the point where the
 * process/program is created. An example of this situation would be a
 * different thread executing an open() call in parallel creating a
 * file-descriptor, which is not a part of the information/state gathered
 * earlier. This can result in unexpected behavior and can cause the
 * program/process to fail.
 *
 * This patch fixes this by acquiring the Wrapper-protection-lock in exclusive
 * mode (write-lock) when executing these wrappers. This guarantees that no
 * other thread would be executing inside a wrapper that can change the process
 * state/information.
 *
 * NOTE:
 * 1. Currently, we do not have WRAPPER_EXECUTION_LOCK/UNLOCK for socket()
 *    family of wrapper. That would be fixed in a later commit.
 * 2. We need to come up with a strategy for certain blocking system calls
 *    that can change the state of the process (e.g. accept).
 * 3. Using trywrlock() can result in starvation if multiple other threads are
 *    rapidly acquiring releasing the lock. For example thread A acquires the
 *    rdlock for 100 ms. Thread B executes and trywrlock and fails. Thread B
 *    sleeps goes to sleep for some time. While thread B is sleeping, thread A
 *    releases the rdlock and reacquires it or some other thread acquires the
 *    rdlock. This would cause the thread B to starve. This scenario can be
 *    easily observed if thread A calls
 *      epoll_wait(fd, events, max_events, -1).
 *    It is wrapped by the epoll_wait wrapper in IPC plugin, which then makes
 *    repeated calls to _real_epoll_wait with smaller timeout.
 */
bool
ThreadSync::wrapperExecutionLockLockExcl()
{
  int saved_errno = errno;
  bool lockAcquired = false;

  if (WorkerState::currentState() == WorkerState::RUNNING ||
      WorkerState::currentState() == WorkerState::PRESUSPEND) {
    incrementWrapperExecutionLockLockCount();
    int retVal = _real_pthread_rwlock_wrlock(&_wrapperExecutionLock);
    if (retVal != 0 && retVal != EDEADLK) {
      fprintf(stderr, "ERROR %s:%d %s: Failed to acquire lock\n",
              __FILE__, __LINE__, __PRETTY_FUNCTION__);
      _exit(DMTCP_FAIL_RC);
    }
    lockAcquired = retVal == 0 ? true : false;
    if (!lockAcquired) {
      decrementWrapperExecutionLockLockCount();
    }
  }
  errno = saved_errno;
  return lockAcquired;
}

// NOTE: Don't do any fancy stuff in this wrapper which can cause the process
// to go into DEADLOCK
void
ThreadSync::wrapperExecutionLockUnlock()
{
  int saved_errno = errno;

  if (_real_pthread_rwlock_unlock(&_wrapperExecutionLock) != 0) {
    fprintf(stderr, "ERROR %s:%d %s: Failed to release lock\n",
            __FILE__, __LINE__, __PRETTY_FUNCTION__);
    _exit(DMTCP_FAIL_RC);
  } else {
    decrementWrapperExecutionLockLockCount();
  }
  errno = saved_errno;
}

bool
ThreadSync::threadCreationLockLock()
{
  int saved_errno = errno;
  bool lockAcquired = false;

  while (1) {
    if (WorkerState::currentState() == WorkerState::RUNNING ||
        WorkerState::currentState() == WorkerState::PRESUSPEND) {
      incrementThreadCreationLockLockCount();
      int retVal = _real_pthread_rwlock_tryrdlock(&_threadCreationLock);
      if (retVal != 1 && retVal == EBUSY) {
        decrementThreadCreationLockLockCount();
        struct timespec sleepTime = { 0, 100 * 1000 * 1000 };
        nanosleep(&sleepTime, NULL);
        continue;
      }
      if (retVal != 0 && retVal != EDEADLK) {
        fprintf(stderr, "ERROR %s:%d %s: Failed to acquire lock\n",
                __FILE__, __LINE__, __PRETTY_FUNCTION__);
        _exit(DMTCP_FAIL_RC);
      }

      // retVal should always be 0 (success) here.
      lockAcquired = retVal == 0 ? true : false;

      // If for some reason, the lock was not acquired, decrement the count
      // that we incremented at the start of this block.
      if (!lockAcquired) {
        decrementThreadCreationLockLockCount();
      }
    }
    break;
  }
  errno = saved_errno;
  return lockAcquired;
}

void
ThreadSync::threadCreationLockUnlock()
{
  int saved_errno = errno;

  if (WorkerState::currentState() != WorkerState::RUNNING &&
      WorkerState::currentState() != WorkerState::PRESUSPEND) {
    fprintf(stderr,
            "DMTCP INTERNAL ERROR: %s:%d %s:\n"
            "       This process is not in RUNNING state and yet this thread\n"
            "       managed to acquire the threadCreationLock.\n"
            "       This should not be happening, something is wrong.",
            __FILE__,
            __LINE__,
            __PRETTY_FUNCTION__);
    _exit(DMTCP_FAIL_RC);
  }
  if (_real_pthread_rwlock_unlock(&_threadCreationLock) != 0) {
    fprintf(stderr, "ERROR %s:%d %s: Failed to release lock\n",
            __FILE__, __LINE__, __PRETTY_FUNCTION__);
    _exit(DMTCP_FAIL_RC);
  } else {
    decrementThreadCreationLockLockCount();
  }
  errno = saved_errno;
}

// GNU g++ uses __thread.  But the C++0x standard says to use thread_local.
// If your compiler fails here, you can: change "__thread" to "thread_local";
// or delete "__thread" (but if user code calls these routines from multiple
// threads, it will not be thread-safe).
// In GCC 4.3 and later, g++ supports -std=c++0x and -std=g++0x.
extern "C"
int
dmtcp_plugin_disable_ckpt()
{
  return ThreadSync::wrapperExecutionLockLock();
}

extern "C"
void
dmtcp_plugin_enable_ckpt()
{
  ThreadSync::wrapperExecutionLockUnlock();
}

void
ThreadSync::waitForThreadsToFinishInitialization()
{
  while (_uninitializedThreadCount != 0) {
    struct timespec sleepTime = { 0, 10 * 1000 * 1000 };
    JTRACE("sleeping")(sleepTime.tv_nsec);
    nanosleep(&sleepTime, NULL);
  }
}

void
ThreadSync::incrementUninitializedThreadCount()
{
  int saved_errno = errno;

  if (WorkerState::currentState() == WorkerState::RUNNING ||
      WorkerState::currentState() == WorkerState::PRESUSPEND) {
    JASSERT(_real_pthread_mutex_lock(&uninitializedThreadCountLock) == 0)
      (JASSERT_ERRNO);
    _uninitializedThreadCount++;

    // JTRACE(":") (_uninitializedThreadCount);
    JASSERT(_real_pthread_mutex_unlock(&uninitializedThreadCountLock) == 0)
      (JASSERT_ERRNO);
  }
  errno = saved_errno;
}

void
ThreadSync::decrementUninitializedThreadCount()
{
  int saved_errno = errno;

  if (WorkerState::currentState() == WorkerState::RUNNING ||
      WorkerState::currentState() == WorkerState::PRESUSPEND) {
    JASSERT(_real_pthread_mutex_lock(&uninitializedThreadCountLock) == 0)
      (JASSERT_ERRNO);
    JASSERT(_uninitializedThreadCount > 0) (_uninitializedThreadCount);
    _uninitializedThreadCount--;

    // JTRACE(":") (_uninitializedThreadCount);
    JASSERT(_real_pthread_mutex_unlock(&uninitializedThreadCountLock) == 0)
      (JASSERT_ERRNO);
  }
  errno = saved_errno;
}

void
ThreadSync::threadFinishedInitialization()
{
  // The following line is to make sure the thread-local data is initialized
  // before any wrapper call is made.
  _hasThreadFinishedInitialization = false;
  decrementUninitializedThreadCount();
  _hasThreadFinishedInitialization = true;
}
