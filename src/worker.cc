// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// worker.cc : individual tasks that can be run in combination to
// stress the system

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// These are necessary, but on by default
// #define __USE_GNU
// #define __USE_LARGEFILE64
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/unistd.h>  // for gettid
#include <netdb.h>
#include <sys/socket.h>

// For size of block device
#include <linux/fs.h>
#include <sys/ioctl.h>
// For asynchronous I/O
#ifdef HAVE_LIBAIO_H
#include <libaio.h>
#endif

#include <sys/syscall.h>

#include <set>
#include <string>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "absl/strings/str_format.h"
#include "ocpdiag/core/results/data_model/input_model.h"
#include "ocpdiag/core/results/measurement_series.h"
#include "ocpdiag/core/results/test_step.h"
#include "os.h"        // NOLINT
#include "pattern.h"   // NOLINT
#include "queue.h"     // NOLINT
#include "sat.h"       // NOLINT
#include "sattypes.h"  // NOLINT
#include "worker.h"    // NOLINT

using ::ocpdiag::results::Diagnosis;
using ::ocpdiag::results::DiagnosisType;
using ::ocpdiag::results::Error;
using ::ocpdiag::results::Log;
using ::ocpdiag::results::LogSeverity;
using ::ocpdiag::results::Measurement;
using ::ocpdiag::results::MeasurementSeries;
using ::ocpdiag::results::MeasurementSeriesElement;
using ::ocpdiag::results::MeasurementSeriesStart;
using ::ocpdiag::results::TestStep;
using ::ocpdiag::results::Validator;
using ::ocpdiag::results::ValidatorType;

// Syscalls
// Why ubuntu, do you hate gettid so bad?
#if !defined(__NR_gettid)
#define __NR_gettid 224
#endif

#define gettid() syscall(__NR_gettid)
#if !defined(CPU_SETSIZE)
_syscall3(int, sched_getaffinity, pid_t, pid, unsigned int, len, cpu_set_t *,
          mask) _syscall3(int, sched_setaffinity, pid_t, pid, unsigned int, len,
                          cpu_set_t *, mask)
#endif

    namespace {
  // Work around the sad fact that there are two (gnu, xsi) incompatible
  // versions of strerror_r floating around google. Awesome.
  bool sat_strerror(int err, char *buf, int len) {
    buf[0] = 0;
    char *errmsg = reinterpret_cast<char *>(strerror_r(err, buf, len));
    int retval = reinterpret_cast<int64>(errmsg);
    if (retval == 0) return true;
    if (retval == -1) return false;
    if (errmsg != buf) {
      strncpy(buf, errmsg, len - 1);
      buf[len - 1] = 0;
    }
    return true;
  }

  inline uint64 addr_to_tag(void *address) {
    return reinterpret_cast<uint64>(address);
  }
}  // namespace

#if !defined(O_DIRECT)
// Sometimes this isn't available.
// Disregard if it's not defined.
#define O_DIRECT 0
#endif

// A struct to hold captured errors, for later reporting.
struct ErrorRecord {
  uint64 actual;     // This is the actual value read.
  uint64 reread;     // This is the actual value, reread.
  uint64 expected;   // This is what it should have been.
  uint64 *vaddr;     // This is where it was (or wasn't).
  char *vbyteaddr;   // This is byte specific where the data was (or wasn't).
  uint64 paddr;      // This is the bus address, if available.
  uint64 *tagvaddr;  // This holds the tag value if this data was tagged.
  uint64 tagpaddr;  // This holds the physical address corresponding to the tag.
  uint32 lastcpu;  // This holds the CPU recorded as probably writing this data.
  const char *patternname;  // This holds the pattern name of the expected data.
};

// This is a helper function to create new threads with pthreads.
static void *ThreadSpawnerGeneric(void *ptr) {
  WorkerThread *worker = static_cast<WorkerThread *>(ptr);
  worker->StartRoutine();
  return NULL;
}

void WorkerStatus::Initialize() {
  sat_assert(0 == pthread_mutex_init(&num_workers_mutex_, NULL));

  pthread_rwlockattr_t attrs;
  sat_assert(0 == pthread_rwlockattr_init(&attrs));
#ifdef HAVE_PTHREAD_RWLOCKATTR_SETKIND_NP
  // Avoid writer lock starvation.
  sat_assert(0 == pthread_rwlockattr_setkind_np(
                      &attrs, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP));
#endif
  sat_assert(0 == pthread_rwlock_init(&status_rwlock_, &attrs));

#ifdef HAVE_PTHREAD_BARRIERS
  sat_assert(0 ==
             pthread_barrier_init(&pause_barrier_, NULL, num_workers_ + 1));
  sat_assert(0 == pthread_rwlock_init(&pause_rwlock_, &attrs));
#endif

  sat_assert(0 == pthread_rwlockattr_destroy(&attrs));
}

void WorkerStatus::Destroy() {
  sat_assert(0 == pthread_mutex_destroy(&num_workers_mutex_));
  sat_assert(0 == pthread_rwlock_destroy(&status_rwlock_));
#ifdef HAVE_PTHREAD_BARRIERS
  sat_assert(0 == pthread_barrier_destroy(&pause_barrier_));
#endif
}

void WorkerStatus::PauseWorkers() {
  if (SetStatus(PAUSE) != PAUSE) WaitOnPauseBarrier();
}

void WorkerStatus::ResumeWorkers() {
  if (SetStatus(RUN) == PAUSE) WaitOnPauseBarrier();
}

void WorkerStatus::StopWorkers() {
  if (SetStatus(STOP) == PAUSE) WaitOnPauseBarrier();
}

bool WorkerStatus::ContinueRunning(bool *paused) {
  // This loop is an optimization.  We use it to immediately re-check the status
  // after resuming from a pause, instead of returning and waiting for the next
  // call to this function.
  if (paused) {
    *paused = false;
  }
  for (;;) {
    switch (GetStatus()) {
      case RUN:
        return true;
      case PAUSE:
        // Wait for the other workers to call this function so that
        // PauseWorkers() can return.
        WaitOnPauseBarrier();
        // Wait for ResumeWorkers() to be called.
        WaitOnPauseBarrier();
        // Indicate that a pause occurred.
        if (paused) {
          *paused = true;
        }
        break;
      case STOP:
        return false;
    }
  }
}

bool WorkerStatus::ContinueRunningNoPause() { return (GetStatus() != STOP); }

void WorkerStatus::RemoveSelf() {
  // Acquire a read lock on status_rwlock_ while (status_ != PAUSE).
  for (;;) {
    AcquireStatusReadLock();
    if (status_ != PAUSE) break;
    // We need to obey PauseWorkers() just like ContinueRunning() would, so that
    // the other threads won't wait on pause_barrier_ forever.
    ReleaseStatusLock();
    // Wait for the other workers to call this function so that PauseWorkers()
    // can return.
    WaitOnPauseBarrier();
    // Wait for ResumeWorkers() to be called.
    WaitOnPauseBarrier();
  }

  // This lock would be unnecessary if we held a write lock instead of a read
  // lock on status_rwlock_, but that would also force all threads calling
  // ContinueRunning() to wait on this one.  Using a separate lock avoids that.
  AcquireNumWorkersLock();
  // Decrement num_workers_ and reinitialize pause_barrier_, which we know isn't
  // in use because (status != PAUSE).
#ifdef HAVE_PTHREAD_BARRIERS
  sat_assert(0 == pthread_barrier_destroy(&pause_barrier_));
  sat_assert(0 == pthread_barrier_init(&pause_barrier_, NULL, num_workers_));
#endif
  --num_workers_;
  ReleaseNumWorkersLock();

  // Release status_rwlock_.
  ReleaseStatusLock();
}

// Parent thread class.
WorkerThread::WorkerThread() {
  status_ = false;
  pages_copied_ = 0;
  errorcount_ = 0;
  runduration_usec_ = 1;
  priority_ = Normal;
  worker_status_ = NULL;
  thread_spawner_ = &ThreadSpawnerGeneric;
  tag_mode_ = false;
}

WorkerThread::~WorkerThread() {}

// Constructors. Just init some default values.
FillThread::FillThread() { num_pages_to_fill_ = 0; }

// Initialize file name to empty.
FileThread::FileThread() {
  filename_ = "";
  pass_ = 0;
  page_io_ = true;
  crc_page_ = -1;
  local_page_ = NULL;
}

// If file thread used bounce buffer in memory, account for the extra
// copy for memory bandwidth calculation.
float FileThread::GetMemoryCopiedData() {
  if (!os_->normal_mem())
    return GetCopiedData();
  else
    return 0;
}

// Initialize target hostname to be invalid.
NetworkThread::NetworkThread() {
  snprintf(ipaddr_, sizeof(ipaddr_), "Unknown");
  sock_ = 0;
}

// Initialize?
NetworkSlaveThread::NetworkSlaveThread() {}

// Initialize?
NetworkListenThread::NetworkListenThread() {}

// Init member variables.
void WorkerThread::InitThread(int thread_num_init, class Sat *sat_init,
                              class OsLayer *os_init,
                              class PatternList *patternlist_init,
                              WorkerStatus *worker_status,
                              ocpdiag::results::TestStep *test_step) {
  sat_assert(worker_status);
  worker_status->AddWorkers(1);

  test_step_ = test_step;
  thread_num_ = thread_num_init;
  sat_ = sat_init;
  os_ = os_init;
  patternlist_ = patternlist_init;
  worker_status_ = worker_status;

  AvailableCpus(&cpu_mask_);
  tag_ = 0xffffffff;

  tag_mode_ = sat_->tag_mode();
}

// Use pthreads to prioritize a system thread.
bool WorkerThread::InitPriority() {
  // This doesn't affect performance that much, and may not be too safe.

  bool ret = BindToCpus(&cpu_mask_);
  if (!ret) {
    AddLog(LogSeverity::kWarning,
           absl::StrFormat("Bind to %s failed",
                           cpuset_format(&cpu_mask_).c_str()));
  }

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Running on core ID %d mask %s (%s)", sched_getcpu(),
                         CurrentCpusFormat().c_str(),
                         cpuset_format(&cpu_mask_).c_str()));
  return true;
}

// Use pthreads to create a system thread.
int WorkerThread::SpawnThread() {
  // Create the new thread.
  int result = pthread_create(&thread_, NULL, thread_spawner_, this);
  if (result) {
    char buf[256];
    sat_strerror(result, buf, sizeof(buf));
    AddProcessError(
        "pthread_create failed when trying to spawn a test thread.");
    status_ = false;
    return false;
  }

  // 0 is pthreads success.
  return true;
}

// Kill the worker thread with SIGINT.
bool WorkerThread::KillThread() { return (pthread_kill(thread_, SIGINT) == 0); }

// Block until thread has exited.
bool WorkerThread::JoinThread() {
  int result = pthread_join(thread_, NULL);

  if (result) {
    AddProcessError(
        absl::StrFormat("pthread_join failed with error code %d.", result));
    status_ = false;
  }

  // 0 is pthreads success.
  return (!result);
}

void WorkerThread::StartRoutine() {
  InitPriority();
  StartThreadTimer();
  Work();
  StopThreadTimer();
  worker_status_->RemoveSelf();
}

// Thread work loop. Execute until marked finished.
bool WorkerThread::Work() {
  do {
    // Sleep for 1 second.
    sat_sleep(1);
  } while (IsReadyToRun());

  return false;
}

// Returns CPU mask of CPUs available to this process,
// Conceptually, each bit represents a logical CPU, ie:
//   mask = 3  (11b):   cpu0, 1
//   mask = 13 (1101b): cpu0, 2, 3
bool WorkerThread::AvailableCpus(cpu_set_t *cpuset) {
  CPU_ZERO(cpuset);
#ifdef HAVE_SCHED_GETAFFINITY
  return sched_getaffinity(getppid(), sizeof(*cpuset), cpuset) == 0;
#else
  return 0;
#endif
}

// Returns CPU mask of CPUs this thread is bound to,
// Conceptually, each bit represents a logical CPU, ie:
//   mask = 3  (11b):   cpu0, 1
//   mask = 13 (1101b): cpu0, 2, 3
bool WorkerThread::CurrentCpus(cpu_set_t *cpuset) {
  CPU_ZERO(cpuset);
#ifdef HAVE_SCHED_GETAFFINITY
  return sched_getaffinity(0, sizeof(*cpuset), cpuset) == 0;
#else
  return 0;
#endif
}

// Bind worker thread to specified CPU(s)
//   Args:
//     thread_mask: cpu_set_t representing CPUs, ie
//                  mask = 1  (01b):   cpu0
//                  mask = 3  (11b):   cpu0, 1
//                  mask = 13 (1101b): cpu0, 2, 3
//
//   Returns true on success, false otherwise.
bool WorkerThread::BindToCpus(const cpu_set_t *thread_mask) {
  cpu_set_t process_mask;
  AvailableCpus(&process_mask);
  if (cpuset_isequal(thread_mask, &process_mask)) return true;
  AddLog(LogSeverity::kDebug, absl::StrFormat("Available CPU mask - %s",
                                              cpuset_format(&process_mask)));
  if (!cpuset_issubset(thread_mask, &process_mask)) {
    // Invalid cpu_mask, ie cpu not allocated to this process or doesn't exist.
    AddLog(LogSeverity::kWarning,
           absl::StrFormat("Requested CPUs %s not a subset of available %s",
                           cpuset_format(thread_mask),
                           cpuset_format(&process_mask)));
    return false;
  }
#ifdef HAVE_SCHED_GETAFFINITY
  if (sat_->use_affinity()) {
    return (sched_setaffinity(gettid(), sizeof(*thread_mask), thread_mask) ==
            0);
  } else {
    AddLog(LogSeverity::kDebug, "Skipping CPU affinity set.");
  }
#endif
  return true;
}

// A worker thread can yield itself to give up CPU until it's scheduled again.
//   Returns true on success, false on error.
bool WorkerThread::YieldSelf() { return (sched_yield() == 0); }

void WorkerThread::AddLog(LogSeverity severity, const string &message) {
  test_step_->AddLog(
      Log{.severity = severity,
          .message = absl::StrFormat("%s #%d: %s", GetThreadTypeName(),
                                     thread_num_, message)});
}

void WorkerThread::AddProcessError(const string &message) {
  test_step_->AddError(
      Error{.symptom = kProcessError,
            .message = absl::StrFormat("%s #%d: %s", GetThreadTypeName(),
                                       thread_num_, message)});
}

void WorkerThread::AddDiagnosis(const string &verdict, DiagnosisType type,
                                const string &message) {
  test_step_->AddDiagnosis(
      Diagnosis{.verdict = verdict,
                .type = type,
                .message = absl::StrFormat("%s #%d: %s", GetThreadTypeName(),
                                           thread_num_, message)});
}

// Fill this page with its pattern.
bool WorkerThread::FillPage(struct page_entry *pe) {
  // Error check arguments.
  if (pe == 0) {
    AddLog(LogSeverity::kError,
           "Attempted to fill a memory page with a null page entry");
    return 0;
  }

  // Tag this page as written from the current CPU.
  pe->lastcpu = sched_getcpu();

  // Mask is the bitmask of indexes used by the pattern.
  // It is the pattern size -1. Size is always a power of 2.
  uint64 *memwords = static_cast<uint64 *>(pe->addr);
  int length = sat_->page_length();

  if (tag_mode_) {
    // Select tag or data as appropriate.
    for (int i = 0; i < length / wordsize_; i++) {
      datacast_t data;

      if ((i & 0x7) == 0) {
        data.l64 = addr_to_tag(&memwords[i]);
      } else {
        data.l32.l = pe->pattern->pattern(i << 1);
        data.l32.h = pe->pattern->pattern((i << 1) + 1);
      }
      memwords[i] = data.l64;
    }
  } else {
    // Just fill in untagged data directly.
    for (int i = 0; i < length / wordsize_; i++) {
      datacast_t data;

      data.l32.l = pe->pattern->pattern(i << 1);
      data.l32.h = pe->pattern->pattern((i << 1) + 1);
      memwords[i] = data.l64;
    }
  }

  return 1;
}

// Tell the thread how many pages to fill.
void FillThread::SetFillPages(int64 num_pages_to_fill_init) {
  num_pages_to_fill_ = num_pages_to_fill_init;
}

// Fill this page with a random pattern.
bool FillThread::FillPageRandom(struct page_entry *pe) {
  // Error check arguments.
  if (pe == 0) {
    AddLog(LogSeverity::kError, "Attempted to fill a null page entry");
    return 0;
  }
  if ((patternlist_ == 0) || (patternlist_->Size() == 0)) {
    AddLog(LogSeverity::kError,
           "No data patterns available when filling memory pages");
    return 0;
  }

  // Choose a random pattern for this block.
  pe->pattern = patternlist_->GetRandomPattern();
  pe->lastcpu = sched_getcpu();

  if (pe->pattern == 0) {
    AddLog(LogSeverity::kError,
           "Attempted to fill a memory page with a null memory pattern");
    return 0;
  }

  // Actually fill the page.
  return FillPage(pe);
}

// Memory fill work loop. Execute until alloted pages filled.
bool FillThread::Work() {
  bool result = true;
  AddLog(LogSeverity::kDebug, "Starting memory page fill thread");

  // We want to fill num_pages_to_fill pages, and
  // stop when we've filled that many.
  // We also want to capture early break
  struct page_entry pe;
  int64 loops = 0;
  while (IsReadyToRun() && (loops < num_pages_to_fill_)) {
    result = result && sat_->GetEmpty(&pe, *test_step_);
    if (!result) {
      AddLog(LogSeverity::kError, "Failed to pop pages, exiting thread");
      break;
    }

    // Fill the page with pattern
    result = result && FillPageRandom(&pe);
    if (!result) break;

    // Put the page back on the queue.
    result = result && sat_->PutValid(&pe, *test_step_);
    if (!result) {
      AddLog(LogSeverity::kError, "Failed to push pages, exiting thread");
      break;
    }
    loops++;
  }

  // Fill in thread status.
  pages_copied_ = loops;
  status_ = result;
  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Completed. Status: %s. Filled %d pages.",
                         status_ ? "Success" : "Fail", pages_copied_));
  return result;
}

// Print error information about a data miscompare.
void WorkerThread::ProcessError(struct ErrorRecord *error,
                                const char *message) {
  char dimm_string[256] = "";

  int core_id = sched_getcpu();

  // Determine if this is a write or read error.
  os_->Flush(error->vaddr);
  error->reread = *(error->vaddr);

  char *good = reinterpret_cast<char *>(&(error->expected));
  char *bad = reinterpret_cast<char *>(&(error->actual));

  sat_assert(error->expected != error->actual);
  unsigned int offset = 0;
  for (offset = 0; offset < (sizeof(error->expected) - 1); offset++) {
    if (good[offset] != bad[offset]) break;
  }

  error->vbyteaddr = reinterpret_cast<char *>(error->vaddr) + offset;

  // Find physical address if possible.
  error->paddr = os_->VirtualToPhysical(error->vbyteaddr, *test_step_);

  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->paddr, dimm_string, sizeof(dimm_string));

  // Report parseable error.
  // TODO(b/273815895): Add hwinfo for cpu and dimms
  AddDiagnosis(
      kMemoryCopyFailVerdict, DiagnosisType::kFail,
      absl::StrFormat(
          "%s: miscompare on CPU %d(<-%d) at %p(0x%llx:%s): "
          "read:0x%016llx, reread:0x%016llx expected:0x%016llx. '%s'%s.\n",
          message, core_id, error->lastcpu, error->vaddr, error->paddr,
          dimm_string, error->actual, error->reread, error->expected,
          (error->patternname) ? error->patternname : "None",
          (error->reread == error->expected) ? " read error" : ""));

  // Overwrite incorrect data with correct data to prevent
  // future miscompares when this data is reused.
  *(error->vaddr) = error->expected;
  os_->Flush(error->vaddr);
}

// Print error information about a data miscompare.
void FileThread::ProcessError(struct ErrorRecord *error, const char *message) {
  char dimm_string[256] = "";

  // Determine if this is a write or read error.
  os_->Flush(error->vaddr);
  error->reread = *(error->vaddr);

  char *good = reinterpret_cast<char *>(&(error->expected));
  char *bad = reinterpret_cast<char *>(&(error->actual));

  sat_assert(error->expected != error->actual);
  unsigned int offset = 0;
  for (offset = 0; offset < (sizeof(error->expected) - 1); offset++) {
    if (good[offset] != bad[offset]) break;
  }

  error->vbyteaddr = reinterpret_cast<char *>(error->vaddr) + offset;

  // Find physical address if possible.
  error->paddr = os_->VirtualToPhysical(error->vbyteaddr, *test_step_);

  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->paddr, dimm_string, sizeof(dimm_string));

  // If crc_page_ is valid, ie checking content read back from file,
  // track src/dst memory addresses. Otherwise categorize as general
  // memory miscompare for CRC checking everywhere else.
  string verdict;
  if (crc_page_ != -1) {
    int miscompare_byteoffset = static_cast<char *>(error->vbyteaddr) -
                                static_cast<char *>(page_recs_[crc_page_].dst);
    verdict = kHddMiscompareFailVerdict;
  } else {
    verdict = kGeneralMiscompareFailVerdict;
  }

  AddDiagnosis(
      verdict, DiagnosisType::kFail,
      absl::StrFormat("%s: miscompare at %p(0x%llx:%s): read:0x%016llx, "
                      "reread:0x%016llx expected:0x%016llx\n",
                      message, error->vaddr, error->paddr, dimm_string,
                      error->actual, error->reread, error->expected,
                      (error->patternname) ? error->patternname : "None"));

  // Overwrite incorrect data with correct data to prevent
  // future miscompares when this data is reused.
  *(error->vaddr) = error->expected;
  os_->Flush(error->vaddr);
}

// Do a word by word result check of a region.
// Print errors on mismatches.
int WorkerThread::CheckRegion(void *addr, class Pattern *pattern,
                              uint32 lastcpu, int64 length, int offset,
                              int64 pattern_offset) {
  uint64 *memblock = static_cast<uint64 *>(addr);
  const int kErrorLimit = 128;
  int errors = 0;
  int overflowerrors = 0;  // Count of overflowed errors.
  bool page_error = false;
  string errormessage("Hardware Error");
  struct ErrorRecord
      recorded[kErrorLimit];  // Queued errors for later printing.

  // For each word in the data region.
  for (int i = 0; i < length / wordsize_; i++) {
    uint64 actual = memblock[i];
    uint64 expected;

    // Determine the value that should be there.
    datacast_t data;
    int index = 2 * i + pattern_offset;
    data.l32.l = pattern->pattern(index);
    data.l32.h = pattern->pattern(index + 1);
    expected = data.l64;
    // Check tags if necessary.
    if (tag_mode_ && ((reinterpret_cast<uint64>(&memblock[i]) & 0x3f) == 0)) {
      expected = addr_to_tag(&memblock[i]);
    }

    // If the value is incorrect, save an error record for later printing.
    if (actual != expected) {
      if (errors < kErrorLimit) {
        recorded[errors].actual = actual;
        recorded[errors].expected = expected;
        recorded[errors].vaddr = &memblock[i];
        recorded[errors].patternname = pattern->name();
        recorded[errors].lastcpu = lastcpu;
        errors++;
      } else {
        page_error = true;
        // If we have overflowed the error queue, just print the errors now.
        AddLog(LogSeverity::kDebug,
               "Error record overflow, too many miscompares");
        errormessage = "Page Error";
        break;
      }
    }
  }

  // Find if this is a whole block corruption.
  if (page_error && !tag_mode_) {
    int patsize = patternlist_->Size();
    for (int pat = 0; pat < patsize; pat++) {
      class Pattern *altpattern = patternlist_->GetPattern(pat, *test_step_);
      const int kGood = 0;
      const int kBad = 1;
      const int kGoodAgain = 2;
      const int kNoMatch = 3;
      int state = kGood;
      unsigned int badstart = 0;
      unsigned int badend = 0;

      // Don't match against ourself!
      if (pattern == altpattern) continue;

      for (int i = 0; i < length / wordsize_; i++) {
        uint64 actual = memblock[i];
        datacast_t expected;
        datacast_t possible;

        // Determine the value that should be there.
        int index = 2 * i + pattern_offset;

        expected.l32.l = pattern->pattern(index);
        expected.l32.h = pattern->pattern(index + 1);

        possible.l32.l = pattern->pattern(index);
        possible.l32.h = pattern->pattern(index + 1);

        if (state == kGood) {
          if (actual == expected.l64) {
            continue;
          } else if (actual == possible.l64) {
            badstart = i;
            badend = i;
            state = kBad;
            continue;
          } else {
            state = kNoMatch;
            break;
          }
        } else if (state == kBad) {
          if (actual == possible.l64) {
            badend = i;
            continue;
          } else if (actual == expected.l64) {
            state = kGoodAgain;
            continue;
          } else {
            state = kNoMatch;
            break;
          }
        } else if (state == kGoodAgain) {
          if (actual == expected.l64) {
            continue;
          } else {
            state = kNoMatch;
            break;
          }
        }
      }

      if ((state == kGoodAgain) || (state == kBad)) {
        unsigned int blockerrors = badend - badstart + 1;
        errormessage = "Block Error";
        // It's okay for the 1st entry to be corrected multiple times,
        // it will simply be reported twice. Once here and once below
        // when processing the error queue.
        ProcessError(&recorded[0], errormessage.c_str());
        AddLog(LogSeverity::kError,
               absl::StrFormat("Block Error: (%p) pattern %s instead of %s, %d "
                               "bytes from offset 0x%x to 0x%x\n",
                               &memblock[badstart], altpattern->name(),
                               pattern->name(), blockerrors * wordsize_,
                               offset + badstart * wordsize_,
                               offset + badend * wordsize_));
      }
    }
  }

  // Process error queue after all errors have been recorded.
  for (int err = 0; err < errors; err++)
    ProcessError(&recorded[err], errormessage.c_str());

  if (page_error) {
    // For each word in the data region.
    for (int i = 0; i < length / wordsize_; i++) {
      uint64 actual = memblock[i];
      uint64 expected;
      datacast_t data;
      // Determine the value that should be there.
      int index = 2 * i + pattern_offset;

      data.l32.l = pattern->pattern(index);
      data.l32.h = pattern->pattern(index + 1);
      expected = data.l64;

      // Check tags if necessary.
      if (tag_mode_ && ((reinterpret_cast<uint64>(&memblock[i]) & 0x3f) == 0)) {
        expected = addr_to_tag(&memblock[i]);
      }

      // If the value is incorrect, save an error record for later printing.
      if (actual != expected) {
        // If we have overflowed the error queue, print the errors now.
        struct ErrorRecord er;
        er.actual = actual;
        er.expected = expected;
        er.vaddr = &memblock[i];

        // Do the error printout. This will take a long time and
        // likely change the machine state.
        ProcessError(&er, errormessage.c_str());
        overflowerrors++;
      }
    }
  }

  // Keep track of observed errors.
  errorcount_ += errors + overflowerrors;
  return errors + overflowerrors;
}

float WorkerThread::GetCopiedData() {
  return pages_copied_ * sat_->page_length() / kMegabyte;
}

// Calculate the CRC of a region.
// Result check if the CRC mismatches.
int WorkerThread::CrcCheckPage(struct page_entry *srcpe) {
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int errors = 0;

  const AdlerChecksum *expectedcrc = srcpe->pattern->crc();
  uint64 *memblock = static_cast<uint64 *>(srcpe->addr);
  int blocks = sat_->page_length() / blocksize;
  for (int currentblock = 0; currentblock < blocks; currentblock++) {
    uint64 *memslice = memblock + currentblock * blockwords;

    AdlerChecksum crc;
    if (tag_mode_) {
      AdlerAddrCrcC(memslice, blocksize, &crc, srcpe);
    } else {
      CalculateAdlerChecksum(memslice, blocksize, &crc);
    }

    // If the CRC does not match, we'd better look closer.
    if (!crc.Equals(*expectedcrc)) {
      AddLog(LogSeverity::kDebug,
             absl::StrFormat("CrcCheckPage Falling through to slow compare, "
                             "CRC mismatch %s != %s",
                             crc.ToHexString(), expectedcrc->ToHexString()));
      int errorcount = CheckRegion(memslice, srcpe->pattern, srcpe->lastcpu,
                                   blocksize, currentblock * blocksize, 0);
      if (errorcount == 0) {
        AddLog(
            LogSeverity::kWarning,
            absl::StrFormat(
                "CrcCheckPage CRC mismatch %s != %s, but no miscompares found.",
                crc.ToHexString(), expectedcrc->ToHexString()));
      }
      errors += errorcount;
    }
  }

  // For odd length transfers, we should never hit this.
  int leftovers = sat_->page_length() % blocksize;
  if (leftovers) {
    uint64 *memslice = memblock + blocks * blockwords;
    errors += CheckRegion(memslice, srcpe->pattern, srcpe->lastcpu, leftovers,
                          blocks * blocksize, 0);
  }
  return errors;
}

// Print error information about a data miscompare.
void WorkerThread::ProcessTagError(struct ErrorRecord *error,
                                   const char *message) {
  char dimm_string[256] = "";
  char tag_dimm_string[256] = "";
  bool read_error = false;

  int core_id = sched_getcpu();

  // Determine if this is a write or read error.
  os_->Flush(error->vaddr);
  error->reread = *(error->vaddr);

  // Distinguish read and write errors.
  if (error->actual != error->reread) {
    read_error = true;
  }

  sat_assert(error->expected != error->actual);

  error->vbyteaddr = reinterpret_cast<char *>(error->vaddr);

  // Find physical address if possible.
  error->paddr = os_->VirtualToPhysical(error->vbyteaddr, *test_step_);
  error->tagpaddr = os_->VirtualToPhysical(error->tagvaddr, *test_step_);

  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->paddr, dimm_string, sizeof(dimm_string));
  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->tagpaddr, tag_dimm_string, sizeof(tag_dimm_string));

  // Report parseable error.
  // TODO(b/273815895): Add hwinfo for cpu and dimms
  AddDiagnosis(kMemoryCopyFailVerdict, DiagnosisType::kFail,
               absl::StrFormat(
                   "%s: Tag from %p(0x%llx:%s) (%s) "
                   "miscompare on CPU %d(0x%s) at %p(0x%llx:%s): "
                   "read:0x%016llx, reread:0x%016llx expected:0x%016llx\n",
                   message, error->tagvaddr, error->tagpaddr, tag_dimm_string,
                   read_error ? "read error" : "write error", core_id,
                   CurrentCpusFormat(), error->vaddr, error->paddr, dimm_string,
                   error->actual, error->reread, error->expected));

  errorcount_ += 1;

  // Overwrite incorrect data with correct data to prevent
  // future miscompares when this data is reused.
  *(error->vaddr) = error->expected;
  os_->Flush(error->vaddr);
}

// Print out and log a tag error.
bool WorkerThread::ReportTagError(uint64 *mem64, uint64 actual, uint64 tag) {
  struct ErrorRecord er;
  er.actual = actual;

  er.expected = tag;
  er.vaddr = mem64;

  // Generate vaddr from tag.
  er.tagvaddr = reinterpret_cast<uint64 *>(actual);

  ProcessTagError(&er, "Hardware Error");
  return true;
}

// C implementation of Adler memory copy, with memory tagging.
bool WorkerThread::AdlerAddrMemcpyC(uint64 *dstmem64, uint64 *srcmem64,
                                    unsigned int size_in_bytes,
                                    AdlerChecksum *checksum,
                                    struct page_entry *pe) {
  // Use this data wrapper to access memory with 64bit read/write.
  datacast_t data;
  datacast_t dstdata;
  unsigned int count = size_in_bytes / sizeof(data);

  if (count > ((1U) << 19)) {
    // Size is too large, must be strictly less than 512 KB.
    return false;
  }

  uint64 a1 = 1;
  uint64 a2 = 1;
  uint64 b1 = 0;
  uint64 b2 = 0;

  class Pattern *pattern = pe->pattern;

  unsigned int i = 0;
  while (i < count) {
    // Process 64 bits at a time.
    if ((i & 0x7) == 0) {
      data.l64 = srcmem64[i];
      dstdata.l64 = dstmem64[i];
      uint64 src_tag = addr_to_tag(&srcmem64[i]);
      uint64 dst_tag = addr_to_tag(&dstmem64[i]);
      // Detect if tags have been corrupted.
      if (data.l64 != src_tag) ReportTagError(&srcmem64[i], data.l64, src_tag);
      if (dstdata.l64 != dst_tag)
        ReportTagError(&dstmem64[i], dstdata.l64, dst_tag);

      data.l32.l = pattern->pattern(i << 1);
      data.l32.h = pattern->pattern((i << 1) + 1);
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;

      data.l64 = dst_tag;
      dstmem64[i] = data.l64;

    } else {
      data.l64 = srcmem64[i];
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;
      dstmem64[i] = data.l64;
    }
    i++;

    data.l64 = srcmem64[i];
    a2 = a2 + data.l32.l;
    b2 = b2 + a2;
    a2 = a2 + data.l32.h;
    b2 = b2 + a2;
    dstmem64[i] = data.l64;
    i++;
  }
  checksum->Set(a1, a2, b1, b2);
  return true;
}

// x86_64 SSE2 assembly implementation of Adler memory copy, with address
// tagging added as a second step. This is useful for debugging failures
// that only occur when SSE / nontemporal writes are used.
bool WorkerThread::AdlerAddrMemcpyWarm(uint64 *dstmem64, uint64 *srcmem64,
                                       unsigned int size_in_bytes,
                                       AdlerChecksum *checksum,
                                       struct page_entry *pe) {
  // Do ASM copy, ignore checksum.
  AdlerChecksum ignored_checksum;
  os_->AdlerMemcpyWarm(dstmem64, srcmem64, size_in_bytes, &ignored_checksum);

  // Force cache flush of both the source and destination addresses.
  //  length - length of block to flush in cachelines.
  //  mem_increment - number of dstmem/srcmem values per cacheline.
  int length = size_in_bytes / kCacheLineSize;
  int mem_increment = kCacheLineSize / sizeof(*dstmem64);
  OsLayer::FastFlushSync();
  for (int i = 0; i < length; ++i) {
    OsLayer::FastFlushHint(dstmem64 + (i * mem_increment));
    OsLayer::FastFlushHint(srcmem64 + (i * mem_increment));
  }
  OsLayer::FastFlushSync();

  // Check results.
  AdlerAddrCrcC(srcmem64, size_in_bytes, checksum, pe);
  // Patch up address tags.
  TagAddrC(dstmem64, size_in_bytes);
  return true;
}

// Retag pages..
bool WorkerThread::TagAddrC(uint64 *memwords, unsigned int size_in_bytes) {
  // Mask is the bitmask of indexes used by the pattern.
  // It is the pattern size -1. Size is always a power of 2.

  // Select tag or data as appropriate.
  int length = size_in_bytes / wordsize_;
  for (int i = 0; i < length; i += 8) {
    datacast_t data;
    data.l64 = addr_to_tag(&memwords[i]);
    memwords[i] = data.l64;
  }
  return true;
}

// C implementation of Adler memory crc.
bool WorkerThread::AdlerAddrCrcC(uint64 *srcmem64, unsigned int size_in_bytes,
                                 AdlerChecksum *checksum,
                                 struct page_entry *pe) {
  // Use this data wrapper to access memory with 64bit read/write.
  datacast_t data;
  unsigned int count = size_in_bytes / sizeof(data);

  if (count > ((1U) << 19)) {
    // Size is too large, must be strictly less than 512 KB.
    return false;
  }

  uint64 a1 = 1;
  uint64 a2 = 1;
  uint64 b1 = 0;
  uint64 b2 = 0;

  class Pattern *pattern = pe->pattern;

  unsigned int i = 0;
  while (i < count) {
    // Process 64 bits at a time.
    if ((i & 0x7) == 0) {
      data.l64 = srcmem64[i];
      uint64 src_tag = addr_to_tag(&srcmem64[i]);
      // Check that tags match expected.
      if (data.l64 != src_tag) ReportTagError(&srcmem64[i], data.l64, src_tag);

      data.l32.l = pattern->pattern(i << 1);
      data.l32.h = pattern->pattern((i << 1) + 1);
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;
    } else {
      data.l64 = srcmem64[i];
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;
    }
    i++;

    data.l64 = srcmem64[i];
    a2 = a2 + data.l32.l;
    b2 = b2 + a2;
    a2 = a2 + data.l32.h;
    b2 = b2 + a2;
    i++;
  }
  checksum->Set(a1, a2, b1, b2);
  return true;
}

// Copy a block of memory quickly, while keeping a CRC of the data.
// Result check if the CRC mismatches.
int WorkerThread::CrcCopyPage(struct page_entry *dstpe,
                              struct page_entry *srcpe) {
  int errors = 0;
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int blocks = sat_->page_length() / blocksize;

  // Base addresses for memory copy
  uint64 *targetmembase = static_cast<uint64 *>(dstpe->addr);
  uint64 *sourcemembase = static_cast<uint64 *>(srcpe->addr);
  // Remember the expected CRC
  const AdlerChecksum *expectedcrc = srcpe->pattern->crc();

  for (int currentblock = 0; currentblock < blocks; currentblock++) {
    uint64 *targetmem = targetmembase + currentblock * blockwords;
    uint64 *sourcemem = sourcemembase + currentblock * blockwords;

    AdlerChecksum crc;
    if (tag_mode_) {
      AdlerAddrMemcpyC(targetmem, sourcemem, blocksize, &crc, srcpe);
    } else {
      AdlerMemcpyC(targetmem, sourcemem, blocksize, &crc);
    }

    // Investigate miscompares.
    if (!crc.Equals(*expectedcrc)) {
      AddLog(LogSeverity::kDebug,
             absl::StrFormat("CrcCopyPage Falling through to slow "
                             "compare, CRC mismatch %s != %s",
                             crc.ToHexString(), expectedcrc->ToHexString()));
      int errorcount = CheckRegion(sourcemem, srcpe->pattern, srcpe->lastcpu,
                                   blocksize, currentblock * blocksize, 0);
      if (errorcount == 0) {
        AddLog(LogSeverity::kWarning,
               absl::StrFormat("CrcCopyPage CRC mismatch %s != %s, but no "
                               "miscompares found. Retrying with fresh data.",
                               crc.ToHexString().c_str(),
                               expectedcrc->ToHexString().c_str()));
        if (!tag_mode_) {
          // Copy the data originally read from this region back again.
          // This data should have any corruption read originally while
          // calculating the CRC.
          memcpy(sourcemem, targetmem, blocksize);
          errorcount = CheckRegion(sourcemem, srcpe->pattern, srcpe->lastcpu,
                                   blocksize, currentblock * blocksize, 0);
          if (errorcount == 0) {
            int core_id = sched_getcpu();
            AddLog(
                LogSeverity::kError,
                absl::StrFormat("CPU %d(0x%s) CrcCopyPage CRC mismatch %s != "
                                "%s, but no miscompares found on second pass.",
                                core_id, CurrentCpusFormat().c_str(),
                                crc.ToHexString().c_str(),
                                expectedcrc->ToHexString().c_str()));
            struct ErrorRecord er;
            er.actual = sourcemem[0];
            er.expected = 0xbad00000ull << 32;
            er.vaddr = sourcemem;
            er.lastcpu = srcpe->lastcpu;
            AddLog(LogSeverity::kError,
                   absl::StrFormat("lastCPU is %d\n", srcpe->lastcpu));
            er.patternname = srcpe->pattern->name();
            ProcessError(&er, "Hardware Error");
            errors += 1;
            errorcount_++;
          }
        }
      }
      errors += errorcount;
    }
  }

  // For odd length transfers, we should never hit this.
  int leftovers = sat_->page_length() % blocksize;
  if (leftovers) {
    uint64 *targetmem = targetmembase + blocks * blockwords;
    uint64 *sourcemem = sourcemembase + blocks * blockwords;

    errors += CheckRegion(sourcemem, srcpe->pattern, srcpe->lastcpu, leftovers,
                          blocks * blocksize, 0);
    int leftoverwords = leftovers / wordsize_;
    for (int i = 0; i < leftoverwords; i++) {
      targetmem[i] = sourcemem[i];
    }
  }

  // Update pattern reference to reflect new contents.
  dstpe->pattern = srcpe->pattern;
  dstpe->lastcpu = sched_getcpu();

  // Clean clean clean the errors away.
  if (errors) {
    // TODO(nsanders): Maybe we should patch rather than fill? Filling may
    // cause bad data to be propogated across the page.
    FillPage(dstpe);
  }
  return errors;
}

// Invert a block of memory quickly, traversing downwards.
int InvertThread::InvertPageDown(struct page_entry *srcpe) {
  const int invert_flush_interval = kCacheLineSize / sizeof(unsigned int);
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int blocks = sat_->page_length() / blocksize;

  // Base addresses for memory copy
  unsigned int *iter =
      static_cast<unsigned int *>(srcpe->addr) + (blocks * blockwords);
  unsigned int *rend = static_cast<unsigned int *>(srcpe->addr);

  OsLayer::FastFlushSync();
  while (iter != rend) {
    for (int i = 0; i < invert_flush_interval; ++i) {
      --iter;
      *iter = ~(*iter);
    }
    OsLayer::FastFlushHint(iter);
  }
  OsLayer::FastFlushSync();
  srcpe->lastcpu = sched_getcpu();
  return 0;
}

// Invert a block of memory, traversing upwards.
int InvertThread::InvertPageUp(struct page_entry *srcpe) {
  const int invert_flush_interval = kCacheLineSize / sizeof(unsigned int);
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int blocks = sat_->page_length() / blocksize;

  // Base addresses for memory copy
  unsigned int *iter = static_cast<unsigned int *>(srcpe->addr);
  unsigned int *end =
      static_cast<unsigned int *>(srcpe->addr) + (blocks * blockwords);

  OsLayer::FastFlushSync();
  while (iter != end) {
    for (int i = 0; i < invert_flush_interval; ++i) {
      *iter = ~(*iter);
      ++iter;
    }
    OsLayer::FastFlushHint(iter - invert_flush_interval);
  }
  OsLayer::FastFlushSync();

  srcpe->lastcpu = sched_getcpu();
  return 0;
}

// Copy a block of memory quickly, while keeping a CRC of the data.
// Result check if the CRC mismatches. Warm the CPU while running
int WorkerThread::CrcWarmCopyPage(struct page_entry *dstpe,
                                  struct page_entry *srcpe) {
  int errors = 0;
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int blocks = sat_->page_length() / blocksize;

  // Base addresses for memory copy
  uint64 *targetmembase = static_cast<uint64 *>(dstpe->addr);
  uint64 *sourcemembase = static_cast<uint64 *>(srcpe->addr);
  // Remember the expected CRC
  const AdlerChecksum *expectedcrc = srcpe->pattern->crc();

  for (int currentblock = 0; currentblock < blocks; currentblock++) {
    uint64 *targetmem = targetmembase + currentblock * blockwords;
    uint64 *sourcemem = sourcemembase + currentblock * blockwords;

    AdlerChecksum crc;
    if (tag_mode_) {
      AdlerAddrMemcpyWarm(targetmem, sourcemem, blocksize, &crc, srcpe);
    } else {
      os_->AdlerMemcpyWarm(targetmem, sourcemem, blocksize, &crc);
    }

    // Investigate miscompares.
    if (!crc.Equals(*expectedcrc)) {
      AddLog(LogSeverity::kDebug,
             absl::StrFormat("CrcWarmCopyPage Falling through to slow compare, "
                             "CRC mismatch %s != %s",
                             crc.ToHexString().c_str(),
                             expectedcrc->ToHexString()));
      int errorcount = CheckRegion(sourcemem, srcpe->pattern, srcpe->lastcpu,
                                   blocksize, currentblock * blocksize, 0);
      if (errorcount == 0) {
        AddLog(
            LogSeverity::kWarning,
            absl::StrFormat(
                "CrcWarmCopyPage CRC mismatch expected: %s != actual: %s, but "
                "no miscompares found. Retrying with fresh data.",
                expectedcrc->ToHexString().c_str(), crc.ToHexString().c_str()));
        if (!tag_mode_) {
          // Copy the data originally read from this region back again.
          // This data should have any corruption read originally while
          // calculating the CRC.
          memcpy(sourcemem, targetmem, blocksize);
          errorcount = CheckRegion(sourcemem, srcpe->pattern, srcpe->lastcpu,
                                   blocksize, currentblock * blocksize, 0);
          if (errorcount == 0) {
            int core_id = sched_getcpu();
            AddLog(LogSeverity::kError,
                   absl::StrFormat("CPU %d(0x%s) CrciWarmCopyPage "
                                   "CRC mismatch %s != %s, "
                                   "but no miscompares found on second pass.\n",
                                   core_id, CurrentCpusFormat().c_str(),
                                   crc.ToHexString().c_str(),
                                   expectedcrc->ToHexString().c_str()));
            struct ErrorRecord er;
            er.actual = sourcemem[0];
            er.expected = 0xbad;
            er.vaddr = sourcemem;
            er.lastcpu = srcpe->lastcpu;
            er.patternname = srcpe->pattern->name();
            ProcessError(&er, "Hardware Error");
            errors++;
            errorcount_++;
          }
        }
      }
      errors += errorcount;
    }
  }

  // For odd length transfers, we should never hit this.
  int leftovers = sat_->page_length() % blocksize;
  if (leftovers) {
    uint64 *targetmem = targetmembase + blocks * blockwords;
    uint64 *sourcemem = sourcemembase + blocks * blockwords;

    errors += CheckRegion(sourcemem, srcpe->pattern, srcpe->lastcpu, leftovers,
                          blocks * blocksize, 0);
    int leftoverwords = leftovers / wordsize_;
    for (int i = 0; i < leftoverwords; i++) {
      targetmem[i] = sourcemem[i];
    }
  }

  // Update pattern reference to reflect new contents.
  dstpe->pattern = srcpe->pattern;
  dstpe->lastcpu = sched_getcpu();

  // Clean clean clean the errors away.
  if (errors) {
    // TODO(nsanders): Maybe we should patch rather than fill? Filling may
    // cause bad data to be propogated across the page.
    FillPage(dstpe);
  }
  return errors;
}

// Memory check work loop. Execute until done, then exhaust pages.
bool CheckThread::Work() {
  struct page_entry pe;
  bool result = true;
  int64 loops = 0;

  AddLog(LogSeverity::kDebug, "Starting Check thread");

  // We want to check all the pages, and
  // stop when there aren't any left.
  while (true) {
    result = result && sat_->GetValid(&pe, *test_step_);
    if (!result) {
      if (IsReadyToRunNoPause())
        AddProcessError("check thread failed to pop pages");
      else
        result = true;
      break;
    }

    // Do the result check.
    CrcCheckPage(&pe);

    // Push pages back on the valid queue if we are still going,
    // throw them out otherwise.
    if (IsReadyToRunNoPause())
      result = result && sat_->PutValid(&pe, *test_step_);
    else
      result = result && sat_->PutEmpty(&pe, *test_step_);
    if (!result) {
      AddProcessError("check thread failed to push pages");
      break;
    }
    loops++;
  }

  pages_copied_ = loops;
  status_ = result;
  AddLog(
      LogSeverity::kDebug,
      absl::StrFormat("Check thread completed with status %d, %d pages copied",
                      status_, pages_copied_));
  return result;
}

// Memory copy work loop. Execute until marked done.
bool CopyThread::Work() {
  struct page_entry src;
  struct page_entry dst;
  bool result = true;
  int64 loops = 0;

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Starting memory copy thread. CPU: %s, Mem: %x, "
                         "Warming: %s, Has Vector: %s",
                         cpuset_format(&cpu_mask_), tag_,
                         sat_->warm() ? "Yes" : "No",
                         os_->has_vector() ? "Yes" : "No"));

  while (IsReadyToRun()) {
    // Pop the needed pages.
    result = result && sat_->GetValid(&src, tag_, *test_step_);
    result = result && sat_->GetEmpty(&dst, tag_, *test_step_);
    if (!result) {
      AddProcessError("Failed to pop pages");
      break;
    }

    // Force errors for unittests.
    if (sat_->error_injection()) {
      if ((random() % 50000) == 8) {
        char *addr = reinterpret_cast<char *>(src.addr);
        int offset = random() % sat_->page_length();
        addr[offset] = 0xba;
      }
    }

    // We can use memcpy, or CRC check while we copy.
    if (sat_->warm()) {
      CrcWarmCopyPage(&dst, &src);
    } else if (sat_->strict()) {
      CrcCopyPage(&dst, &src);
    } else {
      memcpy(dst.addr, src.addr, sat_->page_length());
      dst.pattern = src.pattern;
      dst.lastcpu = sched_getcpu();
    }

    result = result && sat_->PutValid(&dst, *test_step_);
    result = result && sat_->PutEmpty(&src, *test_step_);

    // Copy worker-threads yield themselves at the end of each copy loop,
    // to avoid threads from preempting each other in the middle of the inner
    // copy-loop. Cooperations between Copy worker-threads results in less
    // unnecessary cache thrashing (which happens when context-switching in the
    // middle of the inner copy-loop).
    YieldSelf();

    if (!result) {
      AddProcessError("Failed to push pages.");
      break;
    }
    loops++;
  }

  pages_copied_ = loops;
  status_ = result;
  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Status: %s, %d pages copied.",
                         status_ ? "Success" : "Fail", pages_copied_));
  return result;
}

// Memory invert work loop. Execute until marked done.
bool InvertThread::Work() {
  struct page_entry src;
  bool result = true;
  int64 loops = 0;

  AddLog(LogSeverity::kDebug, "Starting memory invert thread");

  while (IsReadyToRun()) {
    // Pop the needed pages.
    result = result && sat_->GetValid(&src, *test_step_);
    if (!result) {
      AddProcessError("Failed to pop pages");
      break;
    }

    if (sat_->strict()) CrcCheckPage(&src);

    // For the same reason CopyThread yields itself (see YieldSelf comment
    // in CopyThread::Work(), InvertThread yields itself after each invert
    // operation to improve cooperation between different worker threads
    // stressing the memory/cache.
    InvertPageUp(&src);
    YieldSelf();
    InvertPageDown(&src);
    YieldSelf();
    InvertPageDown(&src);
    YieldSelf();
    InvertPageUp(&src);
    YieldSelf();

    if (sat_->strict()) CrcCheckPage(&src);

    result = result && sat_->PutValid(&src, *test_step_);
    if (!result) {
      AddProcessError("Failed to push pages");
      break;
    }
    loops++;
  }

  pages_copied_ = loops * 2;
  status_ = result;
  AddLog(LogSeverity::kDebug,
         absl::StrFormat(
             "Invert thread completed with status %d and %d pages copied",
             status_, pages_copied_));
  return result;
}

// Set file name to use for File IO.
void FileThread::SetFile(const string &filename_init) {
  filename_ = filename_init;
}

// Open the file for access.
bool FileThread::OpenFile(int *pfile) {
  int flags = O_RDWR | O_CREAT | O_SYNC;
  int fd = open(filename_.c_str(), flags | O_DIRECT, 0644);
  if (O_DIRECT != 0 && fd < 0 && errno == EINVAL) {
    fd = open(filename_.c_str(), flags, 0644);  // Try without O_DIRECT
    os_->ActivateFlushPageCache(
        *test_step_);  // Not using O_DIRECT fixed EINVAL
  }
  if (fd < 0) {
    AddProcessError(absl::StrFormat("Failed to create file %s", filename_));
    pages_copied_ = 0;
    return false;
  }
  *pfile = fd;
  return true;
}

// Close the file.
bool FileThread::CloseFile(int fd) {
  close(fd);
  return true;
}

// Check sector tagging.
bool FileThread::SectorTagPage(struct page_entry *src, int block) {
  int page_length = sat_->page_length();
  struct FileThread::SectorTag *tag =
      (struct FileThread::SectorTag *)(src->addr);

  // Tag each sector.
  unsigned char magic = ((0xba + thread_num_) & 0xff);
  for (int sec = 0; sec < page_length / 512; sec++) {
    tag[sec].magic = magic;
    tag[sec].block = block & 0xff;
    tag[sec].sector = sec & 0xff;
    tag[sec].pass = pass_ & 0xff;
  }
  return true;
}

bool FileThread::WritePageToFile(int fd, struct page_entry *src) {
  int page_length = sat_->page_length();
  // Fill the file with our data.
  int64 size = write(fd, src->addr, page_length);

  if (size != page_length) {
    AddDiagnosis(kFileWriteFailVerdict, DiagnosisType::kFail,
                 "Failed to write page to file.");
    errorcount_++;
    AddLog(LogSeverity::kWarning,
           "Block Error: file_thread failed to write, bailing");
    return false;
  }
  return true;
}

// Write the data to the file.
bool FileThread::WritePages(int fd) {
  int strict = sat_->strict();

  // Start fresh at beginning of file for each batch of pages.
  lseek(fd, 0, SEEK_SET);
  for (int i = 0; i < sat_->disk_pages(); i++) {
    struct page_entry src;
    if (!GetValidPage(&src)) return false;
    // Save expected pattern.
    page_recs_[i].pattern = src.pattern;
    page_recs_[i].src = src.addr;

    // Check data correctness.
    if (strict) CrcCheckPage(&src);

    SectorTagPage(&src, i);

    bool result = WritePageToFile(fd, &src);

    if (!PutEmptyPage(&src)) return false;

    if (!result) return false;
  }
  return os_->FlushPageCache(
      *test_step_);  // If O_DIRECT worked, this will be a NOP.
}

// Copy data from file into memory block.
bool FileThread::ReadPageFromFile(int fd, struct page_entry *dst) {
  int page_length = sat_->page_length();

  // Do the actual read.
  int64 size = read(fd, dst->addr, page_length);
  if (size != page_length) {
    AddDiagnosis(kFileReadFailVerdict, DiagnosisType::kFail,
                 "Faile to read page from file.");
    AddLog(LogSeverity::kWarning,
           "Block Error: file_thread failed to read, bailing");
    errorcount_++;
    return false;
  }
  return true;
}

// Check sector tagging.
bool FileThread::SectorValidatePage(const struct PageRec &page,
                                    struct page_entry *dst, int block) {
  // Error injection.
  static int calls = 0;
  calls++;

  // Do sector tag compare.
  int firstsector = -1;
  int lastsector = -1;
  bool badsector = false;
  int page_length = sat_->page_length();

  // Cast data block into an array of tagged sectors.
  struct FileThread::SectorTag *tag =
      (struct FileThread::SectorTag *)(dst->addr);

  sat_assert(sizeof(*tag) == 512);

  // Error injection.
  if (sat_->error_injection()) {
    if (calls == 2) {
      for (int badsec = 8; badsec < 17; badsec++) tag[badsec].pass = 27;
    }
    if (calls == 18) {
      (static_cast<int32 *>(dst->addr))[27] = 0xbadda7a;
    }
  }

  // Check each sector for the correct tag we added earlier,
  // then revert the tag to the to normal data pattern.
  unsigned char magic = ((0xba + thread_num_) & 0xff);
  for (int sec = 0; sec < page_length / 512; sec++) {
    // Check magic tag.
    if ((tag[sec].magic != magic) || (tag[sec].block != (block & 0xff)) ||
        (tag[sec].sector != (sec & 0xff)) ||
        (tag[sec].pass != (pass_ & 0xff))) {
      // Offset calculation for tag location.
      int offset = sec * sizeof(SectorTag);
      if (tag[sec].block != (block & 0xff))
        offset += 1 * sizeof(uint8);
      else if (tag[sec].sector != (sec & 0xff))
        offset += 2 * sizeof(uint8);
      else if (tag[sec].pass != (pass_ & 0xff))
        offset += 3 * sizeof(uint8);

      // Run sector tag error through diagnoser for logging and reporting.
      errorcount_ += 1;
      AddDiagnosis(
          kHddSectorTagFailVerdict, DiagnosisType::kFail,
          absl::StrFormat("Sector Error: Sector tag @ 0x%x, pass %d/%d. sec "
                          "%x/%x, block %d/%d, magic %x/%x, File: %s \n",
                          block * page_length + 512 * sec, (pass_ & 0xff),
                          (unsigned int)tag[sec].pass, sec,
                          (unsigned int)tag[sec].sector, block,
                          (unsigned int)tag[sec].block, magic,
                          (unsigned int)tag[sec].magic, filename_));

      // Keep track of first and last bad sector.
      if (firstsector == -1) firstsector = (block * page_length / 512) + sec;
      lastsector = (block * page_length / 512) + sec;
      badsector = true;
    }
    // Patch tag back to proper pattern.
    unsigned int *addr = (unsigned int *)(&tag[sec]);
    *addr = dst->pattern->pattern(512 * sec / sizeof(*addr));
  }

  // If we found sector errors:
  if (badsector == true) {
    AddLog(LogSeverity::kWarning,
           absl::StrFormat("File sector miscompare at offset %x-%x. File: %s",
                           firstsector * 512, ((lastsector + 1) * 512) - 1,
                           filename_));

    // Either exit immediately, or patch the data up and continue.
    if (sat_->stop_on_error()) {
      exit(1);
    } else {
      // Patch up bad pages.
      for (int block = (firstsector * 512) / page_length;
           block <= (lastsector * 512) / page_length; block++) {
        unsigned int *memblock = static_cast<unsigned int *>(dst->addr);
        int length = page_length / wordsize_;
        for (int i = 0; i < length; i++) {
          memblock[i] = dst->pattern->pattern(i);
        }
      }
    }
  }
  return true;
}

// Get memory for an incoming data transfer..
bool FileThread::PagePrepare() {
  // We can only do direct IO to SAT pages if it is normal mem.
  page_io_ = os_->normal_mem();

  // Init a local buffer if we need it.
  if (!page_io_) {
#ifdef HAVE_POSIX_MEMALIGN
    int result = posix_memalign(&local_page_, 512, sat_->page_length());
#else
    local_page_ = memalign(512, sat_->page_length());
    int result = (local_page_ == 0);
#endif
    if (result) {
      AddProcessError(absl::StrFormat("memalign returned %d (fail)", result));
      status_ = false;
      return false;
    }
  }
  return true;
}

// Remove memory allocated for data transfer.
bool FileThread::PageTeardown() {
  // Free a local buffer if we need to.
  if (!page_io_) {
    free(local_page_);
  }
  return true;
}

// Get memory for an incoming data transfer..
bool FileThread::GetEmptyPage(struct page_entry *dst) {
  if (page_io_) {
    if (!sat_->GetEmpty(dst, *test_step_)) return false;
  } else {
    dst->addr = local_page_;
    dst->offset = 0;
    dst->pattern = 0;
    dst->lastcpu = 0;
  }
  return true;
}

// Get memory for an outgoing data transfer..
bool FileThread::GetValidPage(struct page_entry *src) {
  struct page_entry tmp;
  if (!sat_->GetValid(&tmp, *test_step_)) return false;
  if (page_io_) {
    *src = tmp;
    return true;
  } else {
    src->addr = local_page_;
    src->offset = 0;
    CrcCopyPage(src, &tmp);
    if (!sat_->PutValid(&tmp, *test_step_)) return false;
  }
  return true;
}

// Throw out a used empty page.
bool FileThread::PutEmptyPage(struct page_entry *src) {
  if (page_io_) {
    if (!sat_->PutEmpty(src, *test_step_)) return false;
  }
  return true;
}

// Throw out a used, filled page.
bool FileThread::PutValidPage(struct page_entry *src) {
  if (page_io_) {
    if (!sat_->PutValid(src, *test_step_)) return false;
  }
  return true;
}

// Copy data from file into memory blocks.
bool FileThread::ReadPages(int fd) {
  int page_length = sat_->page_length();
  int strict = sat_->strict();
  bool result = true;

  // Read our data back out of the file, into it's new location.
  lseek(fd, 0, SEEK_SET);
  for (int i = 0; i < sat_->disk_pages(); i++) {
    struct page_entry dst;
    if (!GetEmptyPage(&dst)) return false;
    // Retrieve expected pattern.
    dst.pattern = page_recs_[i].pattern;
    dst.lastcpu = sched_getcpu();
    // Update page recordpage record.
    page_recs_[i].dst = dst.addr;

    // Read from the file into destination page.
    if (!ReadPageFromFile(fd, &dst)) {
      PutEmptyPage(&dst);
      return false;
    }

    SectorValidatePage(page_recs_[i], &dst, i);

    // Ensure that the transfer ended up with correct data.
    if (strict) {
      // Record page index currently CRC checked.
      crc_page_ = i;
      int errors = CrcCheckPage(&dst);
      if (errors) {
        AddLog(LogSeverity::kWarning,
               absl::StrFormat(
                   "File miscompare at block %d, offset %x-%x. File: %s\n", i,
                   i * page_length, ((i + 1) * page_length) - 1, filename_));
        result = false;
      }
      crc_page_ = -1;
      errorcount_ += errors;
    }
    if (!PutValidPage(&dst)) return false;
  }
  return result;
}

// File IO work loop. Execute until marked done.
bool FileThread::Work() {
  bool result = true;
  int64 loops = 0;

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Starting file thread using file: %s", filename_));

  if (!PagePrepare()) {
    status_ = false;
    return false;
  }

  // Open the data IO file.
  int fd = 0;
  if (!OpenFile(&fd)) {
    status_ = false;
    return false;
  }

  pass_ = 0;

  // Load patterns into page records.
  page_recs_ = new struct PageRec[sat_->disk_pages()];
  for (int i = 0; i < sat_->disk_pages(); i++) {
    page_recs_[i].pattern = new class Pattern();
  }

  // Loop until done.
  while (IsReadyToRun()) {
    // Do the file write.
    if (!(result = result && WritePages(fd))) break;

    // Do the file read.
    if (!(result = result && ReadPages(fd))) break;

    loops++;
    pass_ = loops;
  }

  pages_copied_ = loops * sat_->disk_pages();

  // Clean up.
  CloseFile(fd);
  PageTeardown();

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Completed %d: file thread status %d, %d pages copied",
                         thread_num_, status_, pages_copied_));
  // Failure to read from device indicates hardware,
  // rather than procedural SW error.
  status_ = true;
  return true;
}

bool NetworkThread::IsNetworkStopSet() { return !IsReadyToRunNoPause(); }

bool NetworkSlaveThread::IsNetworkStopSet() {
  // This thread has no completion status.
  // It finishes whever there is no more data to be
  // passed back.
  return true;
}

// Set ip name to use for Network IO.
void NetworkThread::SetIP(const char *ipaddr_init) {
  strncpy(ipaddr_, ipaddr_init, 255);
}

// Create a socket.
// Return 0 on error.
bool NetworkThread::CreateSocket(int *psocket) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    AddProcessError("Cannot open socket");
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  *psocket = sock;
  return true;
}

// Close the socket.
bool NetworkThread::CloseSocket(int sock) {
  close(sock);
  return true;
}

// Initiate the tcp connection.
bool NetworkThread::Connect(int sock) {
  struct sockaddr_in dest_addr;
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kNetworkPort);
  memset(&(dest_addr.sin_zero), '\0', sizeof(dest_addr.sin_zero));

  // Translate dot notation to u32.
  if (inet_aton(ipaddr_, &dest_addr.sin_addr) == 0) {
    AddProcessError(absl::StrFormat("Cannot resolve %s", ipaddr_));
    pages_copied_ = 0;
    status_ = false;
    return false;
  }

  if (-1 == connect(sock, reinterpret_cast<struct sockaddr *>(&dest_addr),
                    sizeof(struct sockaddr))) {
    AddProcessError(absl::StrFormat("Cannot connect to %s", ipaddr_));
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  return true;
}

// Initiate the tcp connection.
bool NetworkListenThread::Listen() {
  struct sockaddr_in sa;

  memset(&(sa.sin_zero), '\0', sizeof(sa.sin_zero));

  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = INADDR_ANY;
  sa.sin_port = htons(kNetworkPort);

  if (-1 == ::bind(sock_, (struct sockaddr *)&sa, sizeof(struct sockaddr))) {
    char buf[256];
    sat_strerror(errno, buf, sizeof(buf));
    AddProcessError(absl::StrFormat("Cannot bind socket: %s", buf));
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  listen(sock_, 3);
  return true;
}

// Wait for a connection from a network traffic generation thread.
bool NetworkListenThread::Wait() {
  fd_set rfds;
  struct timeval tv;
  int retval;

  // Watch sock_ to see when it has input.
  FD_ZERO(&rfds);
  FD_SET(sock_, &rfds);
  // Wait up to five seconds.
  tv.tv_sec = 5;
  tv.tv_usec = 0;

  retval = select(sock_ + 1, &rfds, NULL, NULL, &tv);

  return (retval > 0);
}

// Wait for a connection from a network traffic generation thread.
bool NetworkListenThread::GetConnection(int *pnewsock) {
  struct sockaddr_in sa;
  socklen_t size = sizeof(struct sockaddr_in);

  int newsock = accept(sock_, reinterpret_cast<struct sockaddr *>(&sa), &size);
  if (newsock < 0) {
    AddProcessError("Did not receive connection.");
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  *pnewsock = newsock;
  return true;
}

// Send a page, return false if a page was not sent.
bool NetworkThread::SendPage(int sock, struct page_entry *src) {
  int page_length = sat_->page_length();
  char *address = static_cast<char *>(src->addr);

  // Send our data over the network.
  int size = page_length;
  while (size) {
    int transferred = send(sock, address + (page_length - size), size, 0);
    if ((transferred == 0) || (transferred == -1)) {
      if (!IsNetworkStopSet()) {
        char buf[256] = "";
        sat_strerror(errno, buf, sizeof(buf));
        AddProcessError(
            absl::StrFormat("Network write failed with error %s", buf));
        status_ = false;
      }
      return false;
    }
    size = size - transferred;
  }
  return true;
}

// Receive a page. Return false if a page was not received.
bool NetworkThread::ReceivePage(int sock, struct page_entry *dst) {
  int page_length = sat_->page_length();
  char *address = static_cast<char *>(dst->addr);

  // Maybe we will get our data back again, maybe not.
  int size = page_length;
  while (size) {
    int transferred = recv(sock, address + (page_length - size), size, 0);
    if ((transferred == 0) || (transferred == -1)) {
      // Typically network slave thread should exit as network master
      // thread stops sending data.
      if (IsNetworkStopSet()) {
        int err = errno;
        if (transferred == 0 && err == 0) {
          // Two system setups will not sync exactly,
          // allow early exit, but log it.
          AddLog(LogSeverity::kInfo,
                 "Net thread did not receive any data, exiting");
        } else {
          char buf[256] = "";
          sat_strerror(err, buf, sizeof(buf));
          // Print why we failed.
          AddProcessError(
              absl::StrFormat("Network read failed with error %s", buf));
          status_ = false;
          // Print arguments and results.
          AddLog(LogSeverity::kError,
                 absl::StrFormat(
                     "recv(%d, address %x, size %x, 0) == %x, err %d", sock,
                     address + (page_length - size), size, transferred, err));
          if ((transferred == 0) && (page_length - size < 512) &&
              (page_length - size > 0)) {
            // Print null terminated data received, to see who's been
            // sending us supicious unwanted data.
            address[page_length - size] = 0;
            AddLog(LogSeverity::kError,
                   absl::StrFormat("received %d bytes: '%s'",
                                   page_length - size, address));
          }
        }
      }
      return false;
    }
    size = size - transferred;
  }
  return true;
}

// Network IO work loop. Execute until marked done.
// Return true if the thread ran as expected.
bool NetworkThread::Work() {
  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Starting network thread on ip %s", ipaddr_));

  // Make a socket.
  int sock = 0;
  if (!CreateSocket(&sock)) return false;

  // Network IO loop requires network slave thread to have already initialized.
  // We will sleep here for awhile to ensure that the slave thread will be
  // listening by the time we connect.
  // Sleep for 15 seconds.
  sat_sleep(15);
  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Starting execution of network thread on ip %s",
                         ipaddr_));

  // Connect to a slave thread.
  if (!Connect(sock)) return false;

  // Loop until done.
  bool result = true;
  int strict = sat_->strict();
  int64 loops = 0;
  while (IsReadyToRun()) {
    struct page_entry src;
    struct page_entry dst;
    result = result && sat_->GetValid(&src, *test_step_);
    result = result && sat_->GetEmpty(&dst, *test_step_);
    if (!result) {
      AddProcessError("Network thread failed to pop pages");
      break;
    }

    // Check data correctness.
    if (strict) CrcCheckPage(&src);

    // Do the network write.
    if (!(result = result && SendPage(sock, &src))) break;

    // Update pattern reference to reflect new contents.
    dst.pattern = src.pattern;
    dst.lastcpu = sched_getcpu();

    // Do the network read.
    if (!(result = result && ReceivePage(sock, &dst))) break;

    // Ensure that the transfer ended up with correct data.
    if (strict) CrcCheckPage(&dst);

    // Return all of our pages to the queue.
    result = result && sat_->PutValid(&dst, *test_step_);
    result = result && sat_->PutEmpty(&src, *test_step_);
    if (!result) {
      AddProcessError("Network thread failed to push pages");
      break;
    }
    loops++;
  }

  pages_copied_ = loops;
  status_ = result;

  // Clean up.
  CloseSocket(sock);

  AddLog(LogSeverity::kDebug,
         absl::StrFormat(
             "Network thread completed with status %d, %d pages copied",
             status_, pages_copied_));
  return result;
}

// Spawn slave threads for incoming connections.
bool NetworkListenThread::SpawnSlave(int newsock, int threadid) {
  AddLog(LogSeverity::kDebug,
         "Listen thread spawning child thread to handle connection");

  // Spawn slave thread, to reflect network traffic back to sender.
  ChildWorker *child_worker = new ChildWorker;
  child_worker->thread.SetSock(newsock);
  child_worker->thread.InitThread(threadid, sat_, os_, patternlist_,
                                  &child_worker->status, test_step_);
  child_worker->status.Initialize();
  child_worker->thread.SpawnThread();
  child_workers_.push_back(child_worker);

  return true;
}

// Reap slave threads.
bool NetworkListenThread::ReapSlaves() {
  bool result = true;
  // Gather status and reap threads.
  AddLog(LogSeverity::kDebug, "Joining all outstanding threads");

  for (size_t i = 0; i < child_workers_.size(); i++) {
    NetworkSlaveThread &child_thread = child_workers_[i]->thread;
    AddLog(LogSeverity::kDebug, absl::StrFormat("Joining child thread %d", i));
    child_thread.JoinThread();
    if (child_thread.GetStatus() != 1) result = false;
    errorcount_ += child_thread.GetErrorCount();
    AddLog(LogSeverity::kDebug,
           absl::StrFormat("Child thread %d found %lld miscompares", i,
                           child_thread.GetErrorCount()));
    pages_copied_ += child_thread.GetPageCount();
  }

  return result;
}

// Network listener IO work loop. Execute until marked done.
// Return false on fatal software error.
bool NetworkListenThread::Work() {
  AddLog(LogSeverity::kDebug, "Starting network listen thread");

  // Make a socket.
  sock_ = 0;
  if (!CreateSocket(&sock_)) {
    status_ = false;
    return false;
  }
  AddLog(LogSeverity::kDebug, "Listen thread created socket");

  // Allows incoming connections to be queued up by socket library.
  int newsock = 0;
  Listen();
  AddLog(LogSeverity::kDebug, "Listen thread waiting for incoming connections");

  // Wait on incoming connections, and spawn worker threads for them.
  int threadcount = 0;
  while (IsReadyToRun()) {
    // Poll for connections that we can accept().
    if (Wait()) {
      // Accept those connections.
      AddLog(LogSeverity::kDebug,
             "Listen thread found incoming connection, spawning child thread");
      if (GetConnection(&newsock)) {
        SpawnSlave(newsock, threadcount);
        threadcount++;
      }
    }
  }

  // Gather status and join spawned threads.
  ReapSlaves();

  // Delete the child workers.
  for (ChildVector::iterator it = child_workers_.begin();
       it != child_workers_.end(); ++it) {
    (*it)->status.Destroy();
    delete *it;
  }
  child_workers_.clear();

  CloseSocket(sock_);

  status_ = true;
  AddLog(LogSeverity::kDebug,
         absl::StrFormat(
             "Network listen thread completed status %d, %d pages copied",
             status_, pages_copied_));
  return true;
}

// Set network reflector socket struct.
void NetworkSlaveThread::SetSock(int sock) { sock_ = sock; }

// Network reflector IO work loop. Execute until marked done.
// Return false on fatal software error.
bool NetworkSlaveThread::Work() {
  AddLog(LogSeverity::kDebug, "Starting network child thread");

  // Verify that we have a socket.
  int sock = sock_;
  if (!sock) {
    status_ = false;
    return false;
  }

  // Loop until done.
  int64 loops = 0;
  // Init a local buffer for storing data.
  void *local_page = NULL;
#ifdef HAVE_POSIX_MEMALIGN
  int result = posix_memalign(&local_page, 512, sat_->page_length());
#else
  local_page = memalign(512, sat_->page_length());
  int result = (local_page == 0);
#endif
  if (result) {
    AddProcessError(absl::StrFormat(
        "Net slave posix_memalign returned error code %d (fail)", result));
    status_ = false;
    return false;
  }

  struct page_entry page;
  page.addr = local_page;

  // This thread will continue to run as long as the thread on the other end of
  // the socket is still sending and receiving data.
  while (1) {
    // Do the network read.
    if (!ReceivePage(sock, &page)) break;

    // Do the network write.
    if (!SendPage(sock, &page)) break;

    loops++;
  }

  pages_copied_ = loops;
  // No results provided from this type of thread.
  status_ = true;

  // Clean up.
  CloseSocket(sock);

  AddLog(LogSeverity::kDebug,
         absl::StrFormat(
             "Finished network listen child thread, status %d, %d pages copied",
             status_, pages_copied_));
  return true;
}

// Worker thread to heat up CPU.
// This thread does not evaluate pass/fail or software error.
bool CpuStressThread::Work() {
  AddLog(LogSeverity::kDebug, "Starting CPU stress thead");

  do {
    // Run ludloff's platform/CPU-specific assembly workload.
    os_->CpuStressWorkload();
    YieldSelf();
  } while (IsReadyToRun());

  AddLog(LogSeverity::kDebug, "Finished CPU stress thread");
  status_ = true;
  return true;
}

CpuCacheCoherencyThread::CpuCacheCoherencyThread(cc_cacheline_data *data,
                                                 int cacheline_count,
                                                 int thread_num,
                                                 int thread_count,
                                                 int inc_count) {
  cc_cacheline_data_ = data;
  cc_cacheline_count_ = cacheline_count;
  cc_thread_num_ = thread_num;
  cc_thread_count_ = thread_count;
  cc_inc_count_ = inc_count;
}

// A very simple psuedorandom generator.  Since the random number is based
// on only a few simple logic operations, it can be done quickly in registers
// and the compiler can inline it.
uint64 CpuCacheCoherencyThread::SimpleRandom(uint64 seed) {
  return (seed >> 1) ^ (-(seed & 1) & kRandomPolynomial);
}

// Worked thread to test the cache coherency of the CPUs
// Return false on fatal sw error.
bool CpuCacheCoherencyThread::Work() {
  AddLog(LogSeverity::kDebug, "Starting the Cache Coherency thread");
  int64 time_start, time_end;

  // Use a slightly more robust random number for the initial
  // value, so the random sequences from the simple generator will
  // be more divergent.
#ifdef HAVE_RAND_R
  unsigned int seed = static_cast<unsigned int>(gettid());
  uint64 r = static_cast<uint64>(rand_r(&seed));
  r |= static_cast<uint64>(rand_r(&seed)) << 32;
#else
  srand(time(NULL));
  uint64 r = static_cast<uint64>(rand());  // NOLINT
  r |= static_cast<uint64>(rand()) << 32;  // NOLINT
#endif

  time_start = sat_get_time_us();

  uint64 total_inc = 0;  // Total increments done by the thread.
  while (IsReadyToRun()) {
    for (int i = 0; i < cc_inc_count_; i++) {
      // Choose a datastructure in random and increment the appropriate
      // member in that according to the offset (which is the same as the
      // thread number.
      r = SimpleRandom(r);
      int cline_num = r % cc_cacheline_count_;
      int offset;
      // Reverse the order for odd numbered threads in odd numbered cache
      // lines.  This is designed for massively multi-core systems where the
      // number of cores exceeds the bytes in a cache line, so "distant" cores
      // get a chance to exercize cache coherency between them.
      if (cline_num & cc_thread_num_ & 1)
        offset = (cc_thread_count_ & ~1) - cc_thread_num_;
      else
        offset = cc_thread_num_;
      // Increment the member of the randomely selected structure.
      (cc_cacheline_data_[cline_num].num[offset])++;
    }

    total_inc += cc_inc_count_;

    // Calculate if the local counter matches with the global value
    // in all the cache line structures for this particular thread.
    int cc_global_num = 0;
    for (int cline_num = 0; cline_num < cc_cacheline_count_; cline_num++) {
      int offset;
      // Perform the same offset calculation from above.
      if (cline_num & cc_thread_num_ & 1)
        offset = (cc_thread_count_ & ~1) - cc_thread_num_;
      else
        offset = cc_thread_num_;
      cc_global_num += cc_cacheline_data_[cline_num].num[offset];
      // Reset the cachline member's value for the next run.
      cc_cacheline_data_[cline_num].num[offset] = 0;
    }
    if (sat_->error_injection()) cc_global_num = -1;

    // Since the count is only stored in a byte, to squeeze more into a
    // single cache line, only compare it as a byte.  In the event that there
    // is something detected, the chance that it would be missed by a single
    // thread is 1 in 256.  If it affects all cores, that makes the chance
    // of it being missed terribly minute.  It seems unlikely any failure
    // case would be off by more than a small number.
    if ((cc_global_num & 0xff) != (cc_inc_count_ & 0xff)) {
      errorcount_++;
      AddDiagnosis(
          kCacheCoherencyFailVerdict, DiagnosisType::kFail,
          absl::StrFormat(
              "Global (%d) and local (%d) cacheline counters do not match.",
              cc_global_num, cc_inc_count_));
    }
  }
  time_end = sat_get_time_us();

  int64 us_elapsed = time_end - time_start;
  // inc_rate is the no. of increments per second.
  double inc_rate = total_inc * 1e6 / us_elapsed;

  test_step_->AddMeasurement(Measurement{
      .name =
          absl::StrFormat("Cache Coherency Thread %d Runtime", cc_thread_num_),
      .unit = "us",
      .value = static_cast<double>(us_elapsed),
  });
  test_step_->AddMeasurement(Measurement{
      .name = absl::StrFormat("Cache Coherency Thread %d Total Increments",
                              cc_thread_num_),
      .unit = "increments",
      .value = static_cast<double>(total_inc),
  });
  test_step_->AddMeasurement(Measurement{
      .name = absl::StrFormat("Cache Coherency Thread %d Increment Rate",
                              cc_thread_num_),
      .unit = "increment / second",
      .value = inc_rate,
  });
  AddLog(LogSeverity::kDebug, "Finished CPU Cache Coherency thread");
  status_ = true;
  return true;
}

DiskThread::DiskThread(DiskBlockTable *block_table) {
  read_block_size_ = kSectorSize;   // default 1 sector (512 bytes)
  write_block_size_ = kSectorSize;  // this assumes read and write block size
                                    // are the same
  segment_size_ = -1;               // use the entire disk as one segment
  cache_size_ = 16 * 1024 * 1024;   // assume 16MiB cache by default
  // Use a queue such that 3/2 times as much data as the cache can hold
  // is written before it is read so that there is little chance the read
  // data is in the cache.
  queue_size_ = ((cache_size_ / write_block_size_) * 3) / 2;
  blocks_per_segment_ = 32;

  read_threshold_ = 100000;   // 100ms is a reasonable limit for
  write_threshold_ = 100000;  // reading/writing a sector

  read_timeout_ = 5000000;   // 5 seconds should be long enough for a
  write_timeout_ = 5000000;  // timout for reading/writing

  device_sectors_ = 0;
  non_destructive_ = 0;

#ifdef HAVE_LIBAIO_H
  aio_ctx_ = 0;
#endif
  block_table_ = block_table;
  update_block_table_ = 1;

  block_buffer_ = NULL;

  blocks_written_ = 0;
  blocks_read_ = 0;
}

DiskThread::~DiskThread() {
  if (block_buffer_) free(block_buffer_);
}

// Set filename for device file (in /dev).
void DiskThread::SetDevice(const char *device_name) {
  device_name_ = device_name;
}

// Set various parameters that control the behaviour of the test.
// -1 is used as a sentinel value on each parameter (except non_destructive)
// to indicate that the parameter not be set.
bool DiskThread::SetParameters(int read_block_size, int write_block_size,
                               int64 segment_size, int64 cache_size,
                               int blocks_per_segment, int64 read_threshold,
                               int64 write_threshold, int non_destructive) {
  if (read_block_size != -1) {
    // Blocks must be aligned to the disk's sector size.
    if (read_block_size % kSectorSize != 0) {
      AddProcessError(absl::StrFormat(
          "Block size must be a multiple of sector size %d", kSectorSize));
      return false;
    }

    read_block_size_ = read_block_size;
  }

  if (write_block_size != -1) {
    // Write blocks must be aligned to the disk's sector size and to the
    // block size.
    if (write_block_size % kSectorSize != 0) {
      AddProcessError(absl::StrFormat(
          "Write block size must be a multiple of sector size %d",
          kSectorSize));
      return false;
    }
    if (write_block_size % read_block_size_ != 0) {
      AddProcessError(
          absl::StrFormat("Write block size %d must be a multiple of of the "
                          "read block size, which is %d",
                          write_block_size, read_block_size_));
      return false;
    }

    write_block_size_ = write_block_size;

  } else {
    // Make sure write_block_size_ is still valid.
    if (read_block_size_ > write_block_size_) {
      AddLog(LogSeverity::kDebug,
             absl::StrFormat("Assuming write block %d size equal to read block "
                             "size which is %d",
                             write_block_size, read_block_size_));
      write_block_size_ = read_block_size_;
    } else {
      if (write_block_size_ % read_block_size_ != 0) {
        AddProcessError(
            absl::StrFormat("Write block size %d must be a multiple of of the "
                            "read block size, which is %d",
                            write_block_size, read_block_size_));
        return false;
      }
    }
  }

  if (cache_size != -1) {
    cache_size_ = cache_size;
  }

  if (blocks_per_segment != -1) {
    if (blocks_per_segment <= 0) {
      AddProcessError("Blocks per segment must be greater than zero");
      return false;
    }

    blocks_per_segment_ = blocks_per_segment;
  }

  if (read_threshold != -1) {
    if (read_threshold <= 0) {
      AddProcessError("Read threshold must be greater than zero");
      return false;
    }

    read_threshold_ = read_threshold;
  }

  if (write_threshold != -1) {
    if (write_threshold <= 0) {
      AddProcessError("Write threshold must be greater than zero");
      return false;
    }

    write_threshold_ = write_threshold;
  }

  if (segment_size != -1) {
    // Segments must be aligned to the disk's sector size.
    if (segment_size % kSectorSize != 0) {
      AddProcessError(absl::StrFormat(
          "The segment size %d must be a multiple of the sector size %d",
          segment_size, kSectorSize));
      return false;
    }

    segment_size_ = segment_size / kSectorSize;
  }

  non_destructive_ = non_destructive;

  // Having a queue of 150% of blocks that will fit in the disk's cache
  // should be enough to force out the oldest block before it is read and hence,
  // making sure the data comes form the disk and not the cache.
  queue_size_ = ((cache_size_ / write_block_size_) * 3) / 2;
  // Updating DiskBlockTable parameters
  if (update_block_table_) {
    block_table_->SetParameters(kSectorSize, write_block_size_, device_sectors_,
                                segment_size_, device_name_);
  }
  return true;
}

// Open a device, return false on failure.
bool DiskThread::OpenDevice(int *pfile) {
  int flags = O_RDWR | O_SYNC | O_LARGEFILE;
  int fd = open(device_name_.c_str(), flags | O_DIRECT, 0);
  if (O_DIRECT != 0 && fd < 0 && errno == EINVAL) {
    fd = open(device_name_.c_str(), flags, 0);  // Try without O_DIRECT
    os_->ActivateFlushPageCache(*test_step_);
  }
  if (fd < 0) {
    AddProcessError(absl::StrFormat("Failed to open device %s", device_name_));
    return false;
  }
  *pfile = fd;

  return GetDiskSize(fd);
}

// Retrieves the size (in bytes) of the disk/file.
// Return false on failure.
bool DiskThread::GetDiskSize(int fd) {
  struct stat device_stat;
  if (fstat(fd, &device_stat) == -1) {
    AddProcessError(absl::StrFormat("Unable to fstat disk %s", device_name_));
    return false;
  }

  // For a block device, an ioctl is needed to get the size since the size
  // of the device file (i.e. /dev/sdb) is 0.
  if (S_ISBLK(device_stat.st_mode)) {
    uint64 block_size = 0;

    if (ioctl(fd, BLKGETSIZE64, &block_size) == -1) {
      AddProcessError(absl::StrFormat("Unable to ioctl disk %s", device_name_));
      return false;
    }

    // Zero size indicates nonworking device..
    if (block_size == 0) {
      AddDiagnosis(kDeviceSizeZeroFailVerdict, DiagnosisType::kFail,
                   absl::StrFormat("%s has a block size of zero, which "
                                   "indicates a non working device",
                                   device_name_));
      ++errorcount_;
      status_ = true;  // Avoid a procedural error.
      return false;
    }

    device_sectors_ = block_size / kSectorSize;

  } else if (S_ISREG(device_stat.st_mode)) {
    device_sectors_ = device_stat.st_size / kSectorSize;

  } else {
    AddProcessError(
        absl::StrFormat("%s is not a regular file or block", device_name_));
    return false;
  }

  AddLog(LogSeverity::kDebug, absl::StrFormat("Device sectors: %lld on disk %s",
                                              device_sectors_, device_name_));

  if (update_block_table_) {
    block_table_->SetParameters(kSectorSize, write_block_size_, device_sectors_,
                                segment_size_, device_name_);
  }

  return true;
}

bool DiskThread::CloseDevice(int fd) {
  close(fd);
  return true;
}

// Return the time in microseconds.
int64 DiskThread::GetTime() { return sat_get_time_us(); }

// Do randomized reads and (possibly) writes on a device.
// Return false on fatal SW error, true on SW success,
// regardless of whether HW failed.
bool DiskThread::DoWork(int fd) {
  int64 block_num = 0;
  int64 num_segments;

  if (segment_size_ == -1) {
    num_segments = 1;
  } else {
    num_segments = device_sectors_ / segment_size_;
    if (device_sectors_ % segment_size_ != 0) num_segments++;
  }

  // Disk size should be at least 3x cache size.  See comment later for
  // details.
  sat_assert(device_sectors_ * kSectorSize > 3 * cache_size_);

  // This disk test works by writing blocks with a certain pattern to
  // disk, then reading them back and verifying it against the pattern
  // at a later time.  A failure happens when either the block cannot
  // be written/read or when the read block is different than what was
  // written.  If a block takes too long to write/read, then a warning
  // is given instead of an error since taking too long is not
  // necessarily an error.
  //
  // To prevent the read blocks from coming from the disk cache,
  // enough blocks are written before read such that a block would
  // be ejected from the disk cache by the time it is read.
  //
  // TODO(amistry): Implement some sort of read/write throttling.  The
  //                flood of asynchronous I/O requests when a drive is
  //                unplugged is causing the application and kernel to
  //                become unresponsive.

  read_times_ = std::make_unique<MeasurementSeries>(
      MeasurementSeriesStart{
          .name = absl::StrFormat("%s read times", device_name_),
          .unit = "us",
          .validators = {Validator{
              .type = ValidatorType::kLessThanOrEqual,
              .value = {static_cast<double>(read_threshold_)}}},
      },
      *test_step_);
  write_times_ = std::make_unique<MeasurementSeries>(
      MeasurementSeriesStart{
          .name = absl::StrFormat("%s write times", device_name_),
          .unit = "us",
          .validators = {Validator{
              .type = ValidatorType::kLessThanOrEqual,
              .value = {static_cast<double>(write_threshold_)}}},
      },
      *test_step_);

  while (IsReadyToRun()) {
    // Write blocks to disk.
    AddLog(
        LogSeverity::kDebug,
        absl::StrFormat("Write phase %sfor disk %s",
                        non_destructive_ ? "(disabled) " : "", device_name_));
    while (IsReadyToRunNoPause() &&
           in_flight_sectors_.size() < static_cast<size_t>(queue_size_ + 1)) {
      // Confine testing to a particular segment of the disk.
      int64 segment = (block_num / blocks_per_segment_) % num_segments;
      if (!non_destructive_ && (block_num % blocks_per_segment_ == 0)) {
        AddLog(LogSeverity::kDebug,
               absl::StrFormat(
                   "Starting to write segment %lld out of %lld on disk %s",
                   segment, num_segments, device_name_));
      }
      block_num++;

      BlockData *block = block_table_->GetUnusedBlock(segment, *test_step_);

      // If an unused sequence of sectors could not be found, skip to the
      // next block to process.  Soon, a new segment will come and new
      // sectors will be able to be allocated.  This effectively puts a
      // minumim on the disk size at 3x the stated cache size, or 48MiB
      // if a cache size is not given (since the cache is set as 16MiB
      // by default).  Given that todays caches are at the low MiB range
      // and drive sizes at the mid GB, this shouldn't pose a problem.
      // The 3x minimum comes from the following:
      //   1. In order to allocate 'y' blocks from a segment, the
      //      segment must contain at least 2y blocks or else an
      //      allocation may not succeed.
      //   2. Assume the entire disk is one segment.
      //   3. A full write phase consists of writing blocks corresponding to
      //      3/2 cache size.
      //   4. Therefore, the one segment must have 2 * 3/2 * cache
      //      size worth of blocks = 3 * cache size worth of blocks
      //      to complete.
      // In non-destructive mode, don't write anything to disk.
      if (!non_destructive_) {
        if (!WriteBlockToDisk(fd, block)) {
          block_table_->RemoveBlock(block);
          return true;
        }
        blocks_written_++;
      }

      // Block is either initialized by writing, or in nondestructive case,
      // initialized by being added into the datastructure for later reading.
      block->initialized();

      in_flight_sectors_.push(block);
    }
    if (!os_->FlushPageCache(
            *test_step_))  // If O_DIRECT worked, this will be a NOP.
      return false;

    // Verify blocks on disk.
    AddLog(LogSeverity::kDebug,
           absl::StrFormat("Read phase for disk %s", device_name_));
    while (IsReadyToRunNoPause() && !in_flight_sectors_.empty()) {
      BlockData *block = in_flight_sectors_.front();
      in_flight_sectors_.pop();
      if (!ValidateBlockOnDisk(fd, block)) return true;
      block_table_->RemoveBlock(block);
      blocks_read_++;
    }
  }

  pages_copied_ = blocks_written_ + blocks_read_;
  return true;
}

// Do an asynchronous disk I/O operation.
// Return false if the IO is not set up.
bool DiskThread::AsyncDiskIO(IoOp op, int fd, void *buf, int64 size,
                             int64 offset, int64 timeout) {
#ifdef HAVE_LIBAIO_H
  // Use the Linux native asynchronous I/O interface for reading/writing.
  // A read/write consists of three basic steps:
  //    1. create an io context.
  //    2. prepare and submit an io request to the context
  //    3. wait for an event on the context.

  struct {
    const int opcode;
    const char *op_str;
    const char *error_str;
  } operations[2] = {{IO_CMD_PREAD, "read", "disk-read-error"},
                     {IO_CMD_PWRITE, "write", "disk-write-error"}};

  struct iocb cb;
  memset(&cb, 0, sizeof(cb));

  cb.aio_fildes = fd;
  cb.aio_lio_opcode = operations[op].opcode;
  cb.u.c.buf = buf;
  cb.u.c.nbytes = size;
  cb.u.c.offset = offset;

  struct iocb *cbs[] = {&cb};
  if (io_submit(aio_ctx_, 1, cbs) != 1) {
    int error = errno;
    char buf[256];
    sat_strerror(error, buf, sizeof(buf));
    AddProcessError(absl::StrFormat(
        "Unable to submit async %s on disk %s. Error code %d, %s",
        operations[op].op_str, device_name_, error, buf));
    return false;
  }

  struct io_event event;
  memset(&event, 0, sizeof(event));
  struct timespec tv;
  tv.tv_sec = timeout / 1000000;
  tv.tv_nsec = (timeout % 1000000) * 1000;
  if (io_getevents(aio_ctx_, 1, 1, &event, &tv) != 1) {
    // A ctrl-c from the keyboard will cause io_getevents to fail with an
    // EINTR error code.  This is not an error and so don't treat it as such,
    // but still log it.
    int error = errno;
    if (error == EINTR) {
      AddLog(LogSeverity::kDebug,
             absl::StrFormat("%s interrupted on disk %s", operations[op].op_str,
                             device_name_));
    } else {
      AddDiagnosis(
          kDiskAsyncOperationTimeoutFailVerdict, DiagnosisType::kFail,
          absl::StrFormat(
              "Timeout doing async %s to sectors starting at %lld on disk %s",
              operations[op].op_str, offset / kSectorSize, device_name_));
    }

    // Don't bother checking return codes since io_cancel seems to always fail.
    // Since io_cancel is always failing, destroying and recreating an I/O
    // context is a workaround for canceling an in-progress I/O operation.
    // TODO(amistry): Find out why io_cancel isn't working and make it work.
    io_cancel(aio_ctx_, &cb, &event);
    io_destroy(aio_ctx_);
    aio_ctx_ = 0;
    if (io_setup(5, &aio_ctx_)) {
      int error = errno;
      char buf[256];
      sat_strerror(error, buf, sizeof(buf));
      AddProcessError(absl::StrFormat(
          "Unable to create aio context on disk %s Error %d, %s", device_name_,
          error, buf));
    }

    return false;
  }

  // event.res contains the number of bytes written/read or
  // error if < 0, I think.
  if (event.res != static_cast<uint64>(size)) {
    errorcount_++;
    string message;
    string verdict;

    int64 result = static_cast<int64>(event.res);
    if (result < 0) {
      switch (result) {
        case -EIO:
          message = absl::StrFormat(
              "Low-level I/O error while doing %s to sectors starting at %lld "
              "on disk %s",
              operations[op].op_str, offset / kSectorSize, device_name_);
          verdict = kDiskLowLevelIOFailVerdict;
          break;
        default:
          message = absl::StrFormat(
              "Unknown error while doing %s to sectors starting at %lld on "
              "disk %s",
              operations[op].op_str, offset / kSectorSize, device_name_);
          verdict = kDiskUnknownFailVerdict;
      }
    } else {
      message = absl::StrFormat(
          "Unable to %s to sectors starting at %lld on disk %s",
          operations[op].op_str, offset / kSectorSize, device_name_);
      verdict = kDiskUnknownFailVerdict;
    }

    AddDiagnosis(verict, DiagnosisType::kFail, message);
    return false;
  }

  return true;
#else  // !HAVE_LIBAIO_H
  return false;
#endif
}

// Write a block to disk.
// Return false if the block is not written.
bool DiskThread::WriteBlockToDisk(int fd, BlockData *block) {
  memset(block_buffer_, 0, block->size());

  // Fill block buffer with a pattern
  struct page_entry pe;
  if (!sat_->GetValid(&pe, *test_step_)) {
    // Even though a valid page could not be obatined, it is not an error
    // since we can always fill in a pattern directly, albeit slower.
    unsigned int *memblock = static_cast<unsigned int *>(block_buffer_);
    block->set_pattern(patternlist_->GetRandomPattern());

    AddLog(LogSeverity::kWarning,
           absl::StrFormat("Using pattern fill fallback in "
                           "DiskThread::WriteBlockToDisk on disk %s.",
                           device_name_));

    for (unsigned int i = 0; i < block->size() / wordsize_; i++) {
      memblock[i] = block->pattern()->pattern(i);
    }
  } else {
    memcpy(block_buffer_, pe.addr, block->size());
    block->set_pattern(pe.pattern);
    sat_->PutValid(&pe, *test_step_);
  }

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Writing %lld sectors starting at %lld on disk %s",
                         block->size() / kSectorSize, block->address(),
                         device_name_));

  int64 start_time = GetTime();

  if (!AsyncDiskIO(ASYNC_IO_WRITE, fd, block_buffer_, block->size(),
                   block->address() * kSectorSize, write_timeout_)) {
    return false;
  }

  int64 end_time = GetTime();
  write_times_->AddElement(MeasurementSeriesElement{
      .value = static_cast<double>(end_time - start_time)});

  return true;
}

// Verify a block on disk.
// Return true if the block was read, also increment errorcount
// if the block had data errors or performance problems.
bool DiskThread::ValidateBlockOnDisk(int fd, BlockData *block) {
  int64 blocks = block->size() / read_block_size_;
  int64 bytes_read = 0;
  int64 current_blocks;
  int64 current_bytes;
  uint64 address = block->address();

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Reading sectors starting at %lld on disk %s", address,
                         device_name_));

  // Read block from disk and time the read.  If it takes longer than the
  // threshold, complain.
  if (lseek(fd, address * kSectorSize, SEEK_SET) == -1) {
    AddProcessError(
        absl::StrFormat("Unable to seek to sector %lld in "
                        "DiskThread::ValidateSectorsOnDisk on disk %s",
                        address, device_name_));
    return false;
  }
  int64 start_time = GetTime();

  // Split a large write-sized block into small read-sized blocks and
  // read them in groups of randomly-sized multiples of read block size.
  // This assures all data written on disk by this particular block
  // will be tested using a random reading pattern.
  while (blocks != 0) {
    // Test all read blocks in a written block.
    current_blocks = (random() % blocks) + 1;
    current_bytes = current_blocks * read_block_size_;

    memset(block_buffer_, 0, current_bytes);

    AddLog(
        LogSeverity::kDebug,
        absl::StrFormat(
            "Reading %lld sectors starting at sector %lld on disk %s",
            current_bytes / kSectorSize,
            (address * kSectorSize + bytes_read) / kSectorSize, device_name_));

    if (!AsyncDiskIO(ASYNC_IO_READ, fd, block_buffer_, current_bytes,
                     address * kSectorSize + bytes_read, write_timeout_)) {
      return false;
    }

    int64 end_time = GetTime();
    read_times_->AddElement(MeasurementSeriesElement{
        .value = static_cast<double>(end_time - start_time)});

    // In non-destructive mode, don't compare the block to the pattern since
    // the block was never written to disk in the first place.
    if (!non_destructive_) {
      if (CheckRegion(block_buffer_, block->pattern(), 0, current_bytes, 0,
                      bytes_read)) {
        AddDiagnosis(
            kDiskPatternMismatchFailVerdict, DiagnosisType::kFail,
            absl::StrFormat("Pattern mismatch in block starting at sector %lld "
                            "in DiskThread::ValidateSectorsOnDisk on disk %s.",
                            address, device_name_));
      }
    }

    bytes_read += current_blocks * read_block_size_;
    blocks -= current_blocks;
  }

  return true;
}

// Direct device access thread.
// Return false on software error.
bool DiskThread::Work() {
  int fd;

  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Starting disk thread on disk %s", device_name_));

  srandom(time(NULL));

  if (!OpenDevice(&fd)) {
    status_ = false;
    return false;
  }

  // Allocate a block buffer aligned to 512 bytes since the kernel requires it
  // when using direct IO.
#ifdef HAVE_POSIX_MEMALIGN
  int memalign_result =
      posix_memalign(&block_buffer_, kBufferAlignment, sat_->page_length());
#else
  block_buffer_ = memalign(kBufferAlignment, sat_->page_length());
  int memalign_result = (block_buffer_ == 0);
#endif
  if (memalign_result) {
    CloseDevice(fd);
    AddProcessError(
        absl::StrFormat("Unable to allocate memory for buffers for disk %s "
                        "posix memalign returned error code %d.",
                        device_name_, memalign_result));
    status_ = false;
    return false;
  }

#ifdef HAVE_LIBAIO_H
  if (io_setup(5, &aio_ctx_)) {
    CloseDevice(fd);
    AddProcessError(
        absl::StrFormat("Unable to allocate memory for buffers for disk %s "
                        "posix memalign returned error code %d.",
                        device_name_, memalign_result));
    status_ = false;
    return false;
  }
#endif

  bool result = DoWork(fd);

  status_ = result;

#ifdef HAVE_LIBAIO_H
  io_destroy(aio_ctx_);
#endif
  CloseDevice(fd);

  AddLog(LogSeverity::kDebug,
         absl::StrFormat(
             "Completed thread for disk %s: status %d, %d pages copied",
             device_name_, status_, pages_copied_));
  return result;
}

RandomDiskThread::RandomDiskThread(DiskBlockTable *block_table)
    : DiskThread(block_table) {
  update_block_table_ = 0;
}

RandomDiskThread::~RandomDiskThread() {}

// Workload for random disk thread.
bool RandomDiskThread::DoWork(int fd) {
  AddLog(LogSeverity::kDebug,
         absl::StrFormat("Random phase for disk %s", device_name_));
  while (IsReadyToRun()) {
    BlockData *block = block_table_->GetRandomBlock();
    if (block == NULL) {
      AddLog(LogSeverity::kDebug,
             absl::StrFormat("No block available for device %s", device_name_));
    } else {
      ValidateBlockOnDisk(fd, block);
      block_table_->ReleaseBlock(block);
      blocks_read_++;
    }
  }
  pages_copied_ = blocks_read_;
  return true;
}

// The list of MSRs to read from each cpu.
const CpuFreqThread::CpuRegisterType CpuFreqThread::kCpuRegisters[] = {
    {kMsrTscAddr, "TSC"},
    {kMsrAperfAddr, "APERF"},
    {kMsrMperfAddr, "MPERF"},
};

CpuFreqThread::CpuFreqThread(int num_cpus, int freq_threshold, int round)
    : num_cpus_(num_cpus), freq_threshold_(freq_threshold), round_(round) {
  sat_assert(round >= 0);
  if (round == 0) {
    // If rounding is off, force rounding to the nearest MHz.
    round_ = 1;
    round_value_ = 0.5;
  } else {
    round_value_ = round / 2.0;
  }
}

CpuFreqThread::~CpuFreqThread() {}

// Compute the difference between the currently read MSR values and the
// previously read values and store the results in delta. If any of the
// values did not increase, or the TSC value is too small, returns false.
// Otherwise, returns true.
bool CpuFreqThread::ComputeDelta(CpuDataType *current, CpuDataType *previous,
                                 CpuDataType *delta) {
  // Loop through the msrs.
  for (int msr = 0; msr < kMsrLast; msr++) {
    if (previous->msrs[msr] > current->msrs[msr]) {
      AddLog(
          LogSeverity::kWarning,
          absl::StrFormat(
              "Register %s went backwards 0x%llx to 0x%llx skipping interval",
              kCpuRegisters[msr].name, previous->msrs[msr],
              current->msrs[msr]));
      return false;
    } else {
      delta->msrs[msr] = current->msrs[msr] - previous->msrs[msr];
    }
  }

  // Check for TSC < 1 Mcycles over interval.
  if (delta->msrs[kMsrTsc] < (1000 * 1000)) {
    AddLog(LogSeverity::kWarning, "Insanely slow TSC rate, TSC stops in idle?");
    return false;
  }
  timersub(&current->tv, &previous->tv, &delta->tv);

  return true;
}

// Compute the change in values of the MSRs between current and previous,
// set the frequency in MHz of the cpu. If there is an error computing
// the delta, return false. Othewise, return true.
bool CpuFreqThread::ComputeFrequency(CpuDataType *current,
                                     CpuDataType *previous, int *freq) {
  CpuDataType delta;
  if (!ComputeDelta(current, previous, &delta)) {
    return false;
  }

  double interval = delta.tv.tv_sec + delta.tv.tv_usec / 1000000.0;
  double frequency = 1.0 * delta.msrs[kMsrTsc] / 1000000 *
                     delta.msrs[kMsrAperf] / delta.msrs[kMsrMperf] / interval;

  // Use the rounding value to round up properly.
  int computed = static_cast<int>(frequency + round_value_);
  *freq = computed - (computed % round_);
  return true;
}

// This is the task function that the thread executes.
bool CpuFreqThread::Work() {
  cpu_set_t cpuset;
  if (!AvailableCpus(&cpuset)) {
    AddProcessError("Cannot get information about the cpus.");
    return false;
  }

  // Start off indicating the test is passing.
  status_ = true;

  int curr = 0;
  int prev = 1;
  uint32 num_intervals = 0;
  bool paused = false;
  bool valid;
  bool pass = true;

  vector<CpuDataType> data[2];
  data[0].resize(num_cpus_);
  data[1].resize(num_cpus_);

  vector<std::unique_ptr<MeasurementSeries>> cpu_freqs;
  for (int cpu = 0; cpu < num_cpus_; cpu++) {
    cpu_freqs.push_back(std::make_unique<MeasurementSeries>(
        MeasurementSeriesStart{
            .name = absl::StrFormat("CPU Core %d Frequency", cpu),
            .unit = "MHz",
            .validators = {Validator{
                .type = ValidatorType::kGreaterThanOrEqual,
                .value = {static_cast<double>(freq_threshold_)}}},
        },
        *test_step_));
  }

  while (IsReadyToRun(&paused)) {
    if (paused) {
      // Reset the intervals and restart logic after the pause.
      num_intervals = 0;
    }
    if (num_intervals == 0) {
      // If this is the first interval, then always wait a bit before
      // starting to collect data.
      sat_sleep(kStartupDelay);
    }

    // Get the per cpu counters.
    valid = true;
    for (int cpu = 0; cpu < num_cpus_; cpu++) {
      if (CPU_ISSET(cpu, &cpuset)) {
        if (!GetMsrs(cpu, &data[curr][cpu])) {
          AddLog(LogSeverity::kWarning,
                 absl::StrFormat("Failed to get msrs on CPU %d", cpu));
          valid = false;
          break;
        }
      }
    }
    if (!valid) {
      // Reset the number of collected intervals since something bad happened.
      num_intervals = 0;
      continue;
    }

    num_intervals++;

    // Only compute a delta when we have at least two intervals worth of data.
    if (num_intervals > 2) {
      for (int cpu = 0; cpu < num_cpus_; cpu++) {
        if (CPU_ISSET(cpu, &cpuset)) {
          int freq;
          if (!ComputeFrequency(&data[curr][cpu], &data[prev][cpu], &freq)) {
            // Reset the number of collected intervals since an unknown
            // error occurred.
            AddLog(LogSeverity::kWarning,
                   absl::StrFormat("Cannot get frequency of CPU %d", cpu));
            num_intervals = 0;
            break;
          }
          cpu_freqs[cpu]->AddElement(
              MeasurementSeriesElement{.value = static_cast<double>(freq)});
          if (freq < freq_threshold_) {
            errorcount_++;
            pass = false;
            AddDiagnosis(
                kCpuFrequencyTooLowFailVerdict, DiagnosisType::kFail,
                absl::StrFormat("CPU frequency for core %d is too low", cpu));
          }
        }
      }
    }

    sat_sleep(kIntervalPause);

    // Swap the values in curr and prev (these values flip between 0 and 1).
    curr ^= 1;
    prev ^= 1;
  }

  return pass;
}

// Get the MSR values for this particular cpu and save them in data. If
// any error is encountered, returns false. Otherwise, returns true.
bool CpuFreqThread::GetMsrs(int cpu, CpuDataType *data) {
  for (int msr = 0; msr < kMsrLast; msr++) {
    if (!os_->ReadMSR(cpu, kCpuRegisters[msr].msr, &data->msrs[msr],
                      *test_step_)) {
      return false;
    }
  }
  // Save the time at which we acquired these values.
  gettimeofday(&data->tv, NULL);

  return true;
}

// Returns true if this test can run on the current machine. Otherwise,
// returns false.
bool CpuFreqThread::CanRun(TestStep &test_step) {
#if defined(STRESSAPPTEST_CPU_X86_64) || defined(STRESSAPPTEST_CPU_I686)
  unsigned int eax, ebx, ecx, edx;

  // Check that the TSC feature is supported.
  // This check is valid for both Intel and AMD.
  eax = 1;
  cpuid(&eax, &ebx, &ecx, &edx);
  if (!(edx & (1 << 5))) {
    test_step.AddError(Error{
        .symptom = kProcessError,
        .message =
            "Cannot run CPU frequency test. Platform does not support TCS.",
    });
    return false;
  }

  // Check the highest extended function level supported.
  // This check is valid for both Intel and AMD.
  eax = 0x80000000;
  cpuid(&eax, &ebx, &ecx, &edx);
  if (eax < 0x80000007) {
    test_step.AddError(Error{
        .symptom = kProcessError,
        .message = "Cannot run CPU frequency test. Platform does not support "
                   "invariant TCS.",
    });
    return false;
  }

  // Non-Stop TSC is advertised by CPUID.EAX=0x80000007: EDX.bit8
  // This check is valid for both Intel and AMD.
  eax = 0x80000007;
  cpuid(&eax, &ebx, &ecx, &edx);
  if ((edx & (1 << 8)) == 0) {
    test_step.AddError(Error{
        .symptom = kProcessError,
        .message = "Cannot run CPU frequency test. Platform does not support "
                   "non-stop TCS.",
    });
    return false;
  }

  // APERF/MPERF is advertised by CPUID.EAX=0x6: ECX.bit0
  // This check is valid for both Intel and AMD.
  eax = 0x6;
  cpuid(&eax, &ebx, &ecx, &edx);
  if ((ecx & 1) == 0) {
    test_step.AddError(Error{
        .symptom = kProcessError,
        .message = "Cannot run CPU frequency test. Platform does not support "
                   "APERF MSR.",
    });
    return false;
  }
  return true;
#else
  test_step.AddError(Error{
      .symptom = kProcessError,
      .message =
          "Cannot run CPU frequency test. Only supported on x86 platforms.",
  });
  return false;
#endif
}
