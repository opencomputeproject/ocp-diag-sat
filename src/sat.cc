// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// sat.cc : a stress test for stressful testing

// stressapptest (or SAT, from Stressful Application Test) is a test
// designed to stress the system, as well as provide a comprehensive
// memory interface test.

// stressapptest can be run using memory only, or using many system components.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>

#include <memory>

// #define __USE_GNU
// #define __USE_LARGEFILE64
#include <fcntl.h>

#include <list>
#include <string>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "absl/strings/str_format.h"
#include "disk_blocks.h"
#include "logger.h"
#include "ocpdiag/core/results/data_model/dut_info.h"
#include "ocpdiag/core/results/data_model/input_model.h"
#include "ocpdiag/core/results/data_model/input_model_helpers.h"
#include "ocpdiag/core/results/test_step.h"
#include "os.h"
#include "sat.h"
#include "sattypes.h"
#include "worker.h"

using ::ocpdiag::results::Error;
using ::ocpdiag::results::Log;
using ::ocpdiag::results::LogSeverity;
using ::ocpdiag::results::Measurement;
using ::ocpdiag::results::TestStep;
using ::ocpdiag::results::Validator;
using ::ocpdiag::results::ValidatorType;

// stressapptest versioning here.
static const char *kVersion = "1.0.0";

// Global stressapptest reference, for use by signal handler.
// This makes Sat objects not safe for multiple instances.
namespace {
Sat *g_sat = NULL;

// Signal handler for catching break or kill.
//
// This must be installed after g_sat is assigned and while there is a single
// thread.
//
// This must be uninstalled while there is only a single thread, and of course
// before g_sat is cleared or deleted.
void SatHandleBreak(int signal) { g_sat->Break(); }
}  // namespace

// Opens the logfile for writing if necessary
bool Sat::InitializeLogfile() {
  // Open logfile.
  if (use_logfile_) {
    logfile_ = open(logfilename_,
#if defined(O_DSYNC)
                    O_DSYNC |
#elif defined(O_SYNC)
                    O_SYNC |
#elif defined(O_FSYNC)
                    O_FSYNC |
#endif
                        O_WRONLY | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (logfile_ < 0) {
      printf("Fatal Error: cannot open file %s for logging\n", logfilename_);
      bad_status();
      return false;
    }
    // We seek to the end once instead of opening in append mode because no
    // other processes should be writing to it while this one exists.
    if (lseek(logfile_, 0, SEEK_END) == -1) {
      printf("Fatal Error: cannot seek to end of logfile (%s)\n", logfilename_);
      bad_status();
      return false;
    }
    Logger::GlobalLogger()->SetLogFd(logfile_);
  }
  return true;
}

// Check that the environment is known and safe to run on.
// Return 1 if good, 0 if unsuppported.
bool Sat::CheckEnvironment(TestStep &setup_step) {
  // Check that this is not a debug build. Debug builds lack
  // enough performance to stress the system.
#if !defined NDEBUG
  if (run_on_anything_) {
    setup_step.AddLog(Log{
        .severity = LogSeverity::kWarning,
        .message =
            "Running the DEBUG version of SAT. This will significantly reduce "
            "the test's coverage. Do you have the right compiler flags set?"});
  } else {
    setup_step.AddError(
        Error{.symptom = kProcessError,
              .message = "Running the DEBUG version of SAT, which will "
                         "significantly reduce the test's coverage. This error "
                         "can be bypassed with the -A command line flag"});
    return false;
  }
#elif !defined CHECKOPTS
#error Build system regression - COPTS disregarded.
#endif

  // Check if the cpu frequency test is enabled and able to run.
  if (cpu_freq_test_) {
    if (!CpuFreqThread::CanRun(setup_step)) {
      return false;
    } else if (cpu_freq_threshold_ <= 0) {
      setup_step.AddError(
          Error{.symptom = kProcessError,
                .message = "The CPU frequency test requires "
                           "--cpu_freq_threshold be set to a positive value."});
      return false;
    } else if (cpu_freq_round_ < 0) {
      setup_step.AddError(Error{
          .symptom = kProcessError,
          .message = "The --cpu_freq_round option must be greater than or "
                     "equal to zero. A value of zero means no rounding."});
      return false;
    }
  }

  // Use all CPUs if nothing is specified.
  if (memory_threads_ == -1) {
    memory_threads_ = os_->num_cpus();
    setup_step.AddLog(Log{
        .severity = LogSeverity::kDebug,
        .message = absl::StrFormat(
            "Defaulting to using %d memory copy threads (same number as there "
            "are CPU cores)",
            memory_threads_)});
  }

  // Use all memory if no size is specified.
  if (size_mb_ == 0) size_mb_ = os_->FindFreeMemSize(setup_step) / kMegabyte;
  size_ = static_cast<int64>(size_mb_) * kMegabyte;

  // We'd better have some memory by this point.
  if (size_ < 1) {
    setup_step.AddError(
        Error{.symptom = kProcessError,
              .message = "No memory found to test on the system."});
    return false;
  }

  if (tag_mode_ &&
      ((file_threads_ > 0) || (disk_threads_ > 0) || (net_threads_ > 0))) {
    setup_step.AddError(Error{
        .symptom = kProcessError,
        .message =
            "Memory tag mode is incompatible with disk and network testing."});
    return false;
  }

  // If platform is 32 bit Xeon, floor memory size to multiple of 4.
  if (address_mode_ == 32) {
    size_mb_ = (size_mb_ / 4) * 4;
    size_ = size_mb_ * kMegabyte;
    setup_step.AddLog(
        Log{.severity = LogSeverity::kDebug,
            .message = absl::StrFormat(
                "Flooring memory allocation to a multiple of 4: %lld MB",
                size_mb_)});
  }

  return true;
}

// Allocates memory to run the test on
bool Sat::AllocateMemory(TestStep &setup_step) {
  // Allocate our test memory.
  bool result = os_->AllocateTestMem(size_, paddr_base_, setup_step);
  if (!result) {
    setup_step.AddError(
        {Error{.symptom = kProcessError,
               .message = "Failed to allocate memory for test."}});
    return false;
  }
  return true;
}

// Sets up access to data patterns
bool Sat::InitializePatterns(TestStep &setup_step) {
  // Initialize pattern data.
  patternlist_ = new PatternList();
  if (!patternlist_) {
    setup_step.AddError(Error{
        .symptom = kProcessError,
        .message = "Failed to allocate memory patterns.",
    });
    return false;
  }
  if (!patternlist_->Initialize(setup_step)) return false;
  return true;
}

// Get any valid page, no tag specified.
bool Sat::GetValid(struct page_entry *pe, TestStep &test_step) {
  return GetValid(pe, kDontCareTag, test_step);
}

// Fetch and return empty and full pages into the empty and full pools.
bool Sat::GetValid(struct page_entry *pe, int32 tag, TestStep &test_step) {
  bool result = false;
  // Get valid page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    result = finelock_q_->GetValid(pe, tag, test_step);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    result = valid_->PopRandom(pe, test_step);

  if (result) {
    pe->addr =
        os_->PrepareTestMem(pe->offset, page_length_, test_step);  // Map it.

    // Tag this access and current pattern.
    pe->ts = os_->GetTimestamp();
    pe->lastpattern = pe->pattern;

    return (pe->addr != 0);  // Return success or failure.
  }
  return false;
}

bool Sat::PutValid(struct page_entry *pe, TestStep &test_step) {
  if (pe->addr != 0)
    os_->ReleaseTestMem(pe->addr, pe->offset, page_length_,
                        test_step);  // Unmap the page.
  pe->addr = 0;

  // Put valid page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    return finelock_q_->PutValid(pe);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    return valid_->Push(pe);
  else
    return false;
}

// Get an empty page with any tag.
bool Sat::GetEmpty(struct page_entry *pe, TestStep &test_step) {
  return GetEmpty(pe, kDontCareTag, test_step);
}

bool Sat::GetEmpty(struct page_entry *pe, int32 tag, TestStep &test_step) {
  bool result = false;
  // Get empty page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    result = finelock_q_->GetEmpty(pe, tag, test_step);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    result = empty_->PopRandom(pe, test_step);

  if (result) {
    pe->addr =
        os_->PrepareTestMem(pe->offset, page_length_, test_step);  // Map it.
    return (pe->addr != 0);  // Return success or failure.
  }
  return false;
}

bool Sat::PutEmpty(struct page_entry *pe, TestStep &test_step) {
  if (pe->addr != 0)
    os_->ReleaseTestMem(pe->addr, pe->offset, page_length_,
                        test_step);  // Unmap the page.
  pe->addr = 0;

  // Put empty page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    return finelock_q_->PutEmpty(pe);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    return empty_->Push(pe);
  else
    return false;
}

// Set up the bitmap of physical pages in case we want to see which pages were
// accessed under this run of SAT.
void Sat::AddrMapInit(TestStep &fill_step) {
  if (!do_page_map_) return;
  // Find about how much physical mem is in the system.
  // TODO(nsanders): Find some way to get the max
  // and min phys addr in the system.
  uint64 maxsize = os_->FindFreeMemSize(fill_step) * 4;
  sat_assert(maxsize != 0);

  // Make a bitmask of this many pages. Assume that the memory is relatively
  // zero based. This is true on x86, typically.
  // This is one bit per page.
  uint64 arraysize = maxsize / 4096 / 8;
  unsigned char *bitmap = new unsigned char[arraysize];
  sat_assert(bitmap);

  // Mark every page as 0, not seen.
  memset(bitmap, 0, arraysize);

  page_bitmap_size_ = maxsize;
  page_bitmap_ = bitmap;
}

// Add the 4k pages in this block to the array of pages SAT has seen.
void Sat::AddrMapUpdate(struct page_entry *pe, TestStep &fill_step) {
  if (!do_page_map_) return;

  // Go through 4k page blocks.
  uint64 arraysize = page_bitmap_size_ / 4096 / 8;

  char *base = reinterpret_cast<char *>(pe->addr);
  for (int i = 0; i < page_length_; i += 4096) {
    uint64 paddr = os_->VirtualToPhysical(base + i, fill_step);

    uint32 offset = paddr / 4096 / 8;
    unsigned char mask = 1 << ((paddr / 4096) % 8);

    if (offset >= arraysize) {
      fill_step.AddError({Error{
          .symptom = kProcessError,
          .message = absl::StrFormat("Physical address %#llx is greater than "
                                     "the expected limit %#llx.",
                                     paddr, page_bitmap_size_),
      }});
      sat_assert(0);
    }
    page_bitmap_[offset] |= mask;
  }
}

// Print out the physical memory ranges that SAT has accessed.
void Sat::AddrMapPrint(TestStep &fill_step) {
  if (!do_page_map_) return;

  uint64 pages = page_bitmap_size_ / 4096;

  uint64 last_page = 0;
  bool valid_range = false;

  fill_step.AddLog(
      Log{.severity = LogSeverity::kInfo,
          .message =
              "Printing physical memory ranges that this test has accessed."});

  for (uint64 i = 0; i < pages; i++) {
    int offset = i / 8;
    unsigned char mask = 1 << (i % 8);

    bool touched = page_bitmap_[offset] & mask;
    if (touched && !valid_range) {
      valid_range = true;
      last_page = i * 4096;
    } else if (!touched && valid_range) {
      valid_range = false;
      fill_step.AddLog(
          Log{.severity = LogSeverity::kInfo,
              .message = absl::StrFormat("%#016llx - %#016llx", last_page,
                                         (i * 4096) - 1)});
    }
  }
  fill_step.AddLog(Log{.severity = LogSeverity::kInfo,
                       .message = "Done print physical memory ranges."});
}

// Initializes page lists and fills pages with data patterns.
bool Sat::InitializePages() {
  // TODO(b/273821926) Populate fill memory pages step
  auto fill_step =
      std::make_unique<TestStep>("Setup and Fill Memory Pages", *test_run_);

  int result = 1;

  fill_step->AddMeasurement(Measurement{
      .name = "Total Memory Page Count",
      .unit = "pages",
      .value = static_cast<double>(pages_),
  });

  // Calculate needed page totals.
  double neededpages = memory_threads_ + invert_threads_ + check_threads_ +
                       net_threads_ + file_threads_;
  fill_step->AddMeasurement(Measurement{
      .name = "Required Thread Memory Page Count",
      .unit = "pages",
      .value = neededpages,
  });

  // Empty-valid page ratio is adjusted depending on queue implementation.
  // since fine-grain-locked queue keeps both valid and empty entries in the
  // same queue and randomly traverse to find pages, the empty-valid ratio
  // should be more even.
  if (pe_q_implementation_ == SAT_FINELOCK)
    freepages_ = pages_ / 5 * 2;  // Mark roughly 2/5 of all pages as Empty.
  else
    freepages_ = (pages_ / 100) + (2 * neededpages);
  fill_step->AddMeasurement(Measurement{
      .name = "Free Memory Page Count",
      .unit = "pages",
      .validators = ocpdiag::results::ValidateWithinInclusiveLimits(neededpages,
                                                                    pages_ / 2),
      .value = static_cast<double>(freepages_),
  });

  if (freepages_ < neededpages) {
    fill_step->AddError(
        Error{.symptom = kProcessError,
              .message = absl::StrFormat(
                  "The number of required free memory pages is less than the "
                  "number of pages required for the test. This likely means "
                  "that the parameters to the test are not valid. Total Pages: "
                  "%d, Required Pages: %d, Free Pages: %d",
                  pages_, neededpages, freepages_)});
    return false;
  }

  if (freepages_ > pages_ / 2) {
    fill_step->AddError(
        Error{.symptom = kProcessError,
              .message = absl::StrFormat(
                  "The number of required free memory pages is less than the "
                  "number of available pages. This likely means that the "
                  "parameters to the test are not valid. Total Pages: %d, "
                  "Required Pages: %d, Available Pages: %d",
                  pages_, freepages_, pages_ / 2)});
    return false;
  }

  fill_step->AddLog(
      Log{.severity = LogSeverity::kDebug,
          .message = absl::StrFormat(
              "Allocating memory pages, Total Pages: %d, Free Pages: %d",
              pages_, freepages_)});

  // Initialize page locations.
  for (int64 i = 0; i < pages_; i++) {
    struct page_entry pe;
    init_pe(&pe);
    pe.offset = i * page_length_;
    result &= PutEmpty(&pe, *fill_step);
  }

  if (!result) {
    fill_step->AddError(
        Error{.symptom = kProcessError,
              .message = "Error while initializing free memory pages"});
    return false;
  }

  // Fill valid pages with test patterns.
  // Use fill threads to do this.
  WorkerStatus fill_status;
  WorkerVector fill_vector;

  fill_step->AddLog(
      Log{.severity = LogSeverity::kDebug,
          .message = absl::StrFormat(
              "Starting memory page fill threads: %d threads, %d pages",
              fill_threads_, pages_)});
  // Initialize the fill threads.
  for (int i = 0; i < fill_threads_; i++) {
    FillThread *thread = new FillThread();
    thread->InitThread(i, this, os_, patternlist_, &fill_status,
                       fill_step.get());
    if (i != fill_threads_ - 1) {
      fill_step->AddLog(
          Log{.severity = LogSeverity::kDebug,
              .message = absl::StrFormat(
                  "Starting memory page fill Thread %d to fill %d pages", i,
                  pages_ / fill_threads_)});
      thread->SetFillPages(pages_ / fill_threads_);
      // The last thread finishes up all the leftover pages.
    } else {
      fill_step->AddLog(
          Log{.severity = LogSeverity::kDebug,
              .message = absl::StrFormat(
                  "Starting memory page fill Thread %d to fill %d pages", i,
                  pages_ - pages_ / fill_threads_ * i)});
      thread->SetFillPages(pages_ - pages_ / fill_threads_ * i);
    }
    fill_vector.push_back(thread);
  }

  // Spawn the fill threads.
  fill_status.Initialize();
  for (WorkerVector::const_iterator it = fill_vector.begin();
       it != fill_vector.end(); ++it)
    (*it)->SpawnThread();

  // Reap the finished fill threads.
  for (WorkerVector::const_iterator it = fill_vector.begin();
       it != fill_vector.end(); ++it) {
    (*it)->JoinThread();
    if ((*it)->GetStatus() != 1) {
      fill_step->AddError(Error{
          .symptom = kProcessError,
          .message = absl::StrFormat(
              "Memory page fill thread %d failed with status %d after running "
              "for %.2f seconds. See error logs for additional information.",
              (*it)->ThreadID(), (*it)->GetStatus(),
              (*it)->GetRunDurationUSec() * 1.0 / 1000000)});
      return false;
    }
    delete (*it);
  }
  fill_vector.clear();
  fill_status.Destroy();
  fill_step->AddLog(
      Log{.severity = LogSeverity::kDebug,
          .message = "Done filling memory pages. Starting to allocate pages."});

  AddrMapInit(*fill_step);

  // Initialize page locations.
  for (int64 i = 0; i < pages_; i++) {
    struct page_entry pe;
    // Only get valid pages with uninitialized tags here.
    if (GetValid(&pe, kInvalidTag, *fill_step)) {
      int64 paddr = os_->VirtualToPhysical(pe.addr, *fill_step);
      int32 region = os_->FindRegion(paddr, *fill_step);
      region_[region]++;
      pe.paddr = paddr;
      pe.tag = 1 << region;
      region_mask_ |= pe.tag;

      // Generate a physical region map
      AddrMapUpdate(&pe, *fill_step);

      // Note: this does not allocate free pages among all regions
      // fairly. However, with large enough (thousands) random number
      // of pages being marked free in each region, the free pages
      // count in each region end up pretty balanced.
      if (i < freepages_) {
        result &= PutEmpty(&pe, *fill_step);
      } else {
        result &= PutValid(&pe, *fill_step);
      }
    } else {
      fill_step->AddError(Error{
          .symptom = kProcessError,
          .message =
              absl::StrFormat("Error allocating pages. Total Pages: %d, Pages "
                              "Allocated: %d, Pages Not Allocated: %d",
                              pages_, i, pages_ - i)});
      return false;
    }
  }
  fill_step->AddLog(Log{.severity = LogSeverity::kDebug,
                        .message = "Done allocating pages."});

  AddrMapPrint(*fill_step);

  for (int i = 0; i < 32; i++) {
    if (region_mask_ & (1 << i)) {
      region_count_++;
      fill_step->AddLog(Log{.severity = LogSeverity::kDebug,
                            .message = absl::StrFormat(
                                "Region %d corresponds to %d", i, region_[i])});
    }
  }
  fill_step->AddLog(
      Log{.severity = LogSeverity::kDebug,
          .message = absl::StrFormat("Region mask: 0x%x", region_mask_)});

  return true;
}

// Initializes the resources that SAT needs to run.
// This needs to be called before Run(), and after ParseArgs().
// Returns true on success, false on error, and will exit() on help message.
bool Sat::Initialize() {
  test_run_ = std::make_unique<ocpdiag::results::TestRun>(
      ocpdiag::results::TestRunStart{
          .name = "Stress App Test",
          .version = kVersion,
          .command_line = cmdline_,
          .parameters_json = cmdline_json_,
      });

  g_sat = this;

  // Initializes sync'd log file to ensure output is saved.
  if (!InitializeLogfile()) return false;
  Logger::GlobalLogger()->SetTimestampLogging(log_timestamps_);
  Logger::GlobalLogger()->StartThread();

  if (!ValidateArgs()) return false;

  // TODO(b/273815895) Report DUT info
  test_run_->StartAndRegisterDutInfo(
      std::make_unique<ocpdiag::results::DutInfo>("place", "holder"));

  auto setup_step =
      std::make_unique<TestStep>("Setup and Check Environment", *test_run_);

  // Initialize OS/Hardware interface.
  std::map<std::string, std::string> options;
  os_ = OsLayerFactory(options);
  if (!os_) {
    setup_step->AddError(Error{.symptom = kProcessError,
                               .message = "Failed to allocate OS interface."});
    return false;
  }

  // Populate OS parameters
  if (min_hugepages_mbytes_ > 0)
    os_->SetMinimumHugepagesSize(min_hugepages_mbytes_ * kMegabyte);

  if (reserve_mb_ > 0) os_->SetReserveSize(reserve_mb_);

  if (channels_.size() > 0) {
    setup_step->AddLog(
        Log{.severity = LogSeverity::kDebug,
            .message = absl::StrFormat(
                "Decoding memory: %dx%d bit channels, %d modules per "
                "channel (x%d), decoding hash 0x%x",
                channels_.size(), channel_width_, channels_[0].size(),
                channel_width_ / channels_[0].size(), channel_hash_)});
    os_->SetDramMappingParams(channel_hash_, channel_width_, &channels_);
  }

  if (!os_->Initialize(*setup_step)) {
    setup_step->AddError(
        Error{.symptom = kProcessError,
              .message = "Failed to initialize OS interface."});
    delete os_;
    return false;
  }

  // Checks that OS/Build/Platform is supported.
  if (!CheckEnvironment(*setup_step)) return false;

  os_->set_error_injection(error_injection_);

  // Run SAT in monitor only mode, do not continue to allocate resources.
  if (monitor_mode_) {
    setup_step->AddLog(Log{
        .severity = LogSeverity::kInfo,
        .message = "Running in monitor-only mode. THe test will not allocated "
                   "any memory or run any stress testing. It will only poll "
                   "for ECC errors.",
    });
    return true;
  }

  // Allocate the memory to test.
  if (!AllocateMemory(*setup_step)) return false;

  setup_step->AddMeasurement(Measurement{
      .name = "Memory to Test",
      .unit = "MB",
      .value = static_cast<double>(size_ / kMegabyte),
  });
  setup_step->AddMeasurement(Measurement{
      .name = "Test Run Time",
      .unit = "s",
      .value = static_cast<double>(runtime_seconds_),
  });

  if (!InitializePatterns(*setup_step)) return false;

  // Initialize memory allocation.
  pages_ = size_ / page_length_;

  // Allocate page queue depending on queue implementation switch.
  if (pe_q_implementation_ == SAT_FINELOCK) {
    finelock_q_ = new FineLockPEQueue(pages_, page_length_);
    if (finelock_q_ == NULL) return false;
  } else if (pe_q_implementation_ == SAT_ONELOCK) {
    empty_ = new PageEntryQueue(pages_);
    valid_ = new PageEntryQueue(pages_);
    if ((empty_ == NULL) || (valid_ == NULL)) return false;
  }

  setup_step.reset();

  if (!InitializePages()) return false;

  return true;
}

// Constructor and destructor.
Sat::Sat() {
  // Set defaults, command line might override these.
  runtime_seconds_ = 20;
  page_length_ = kSatPageSize;
  disk_pages_ = kSatDiskPage;
  pages_ = 0;
  size_mb_ = 0;
  size_ = size_mb_ * kMegabyte;
  reserve_mb_ = 0;
  min_hugepages_mbytes_ = 0;
  freepages_ = 0;
  paddr_base_ = 0;
  channel_hash_ = kCacheLineSize;
  channel_width_ = 64;

  user_break_ = false;
  verbosity_ = 8;
  Logger::GlobalLogger()->SetVerbosity(verbosity_);
  print_delay_ = 10;
  strict_ = 1;
  warm_ = 0;
  run_on_anything_ = 0;
  use_logfile_ = false;
  logfile_ = 0;
  log_timestamps_ = true;
  // Detect 32/64 bit binary.
  void *pvoid = 0;
  address_mode_ = sizeof(pvoid) * 8;
  error_injection_ = false;
  crazy_error_injection_ = false;
  max_errorcount_ = 0;  // Zero means no early exit.
  stop_on_error_ = false;

  do_page_map_ = false;
  page_bitmap_ = 0;
  page_bitmap_size_ = 0;

  // Cache coherency data initialization.
  cc_test_ = false;         // Flag to trigger cc threads.
  cc_cacheline_count_ = 2;  // Two datastructures of cache line size.
  cc_cacheline_size_ = 0;   // Size of a cacheline (0 for auto-detect).
  cc_inc_count_ = 1000;     // Number of times to increment the shared variable.
  cc_cacheline_data_ = 0;   // Cache Line size datastructure.

  // Cpu frequency data initialization.
  cpu_freq_test_ = false;   // Flag to trigger cpu frequency thread.
  cpu_freq_threshold_ = 0;  // Threshold, in MHz, at which a cpu fails.
  cpu_freq_round_ = 10;     // Round the computed frequency to this value.

  sat_assert(0 == pthread_mutex_init(&worker_lock_, NULL));
  file_threads_ = 0;
  net_threads_ = 0;
  listen_threads_ = 0;
  // Default to autodetect number of cpus, and run that many threads.
  memory_threads_ = -1;
  invert_threads_ = 0;
  fill_threads_ = 8;
  check_threads_ = 0;
  cpu_stress_threads_ = 0;
  disk_threads_ = 0;
  total_threads_ = 0;

  use_affinity_ = true;
  region_mask_ = 0;
  region_count_ = 0;
  for (int i = 0; i < 32; i++) {
    region_[i] = 0;
  }
  region_mode_ = 0;

  errorcount_ = 0;
  statuscount_ = 0;

  valid_ = 0;
  empty_ = 0;
  finelock_q_ = 0;
  // Default to use fine-grain lock for better performance.
  pe_q_implementation_ = SAT_FINELOCK;

  os_ = 0;
  patternlist_ = 0;
  logfilename_[0] = 0;

  read_block_size_ = 512;
  write_block_size_ = -1;
  segment_size_ = -1;
  cache_size_ = -1;
  blocks_per_segment_ = -1;
  read_threshold_ = -1;
  write_threshold_ = -1;
  non_destructive_ = 1;
  monitor_mode_ = 0;
  tag_mode_ = 0;
  random_threads_ = 0;

  pause_delay_ = 600;
  pause_duration_ = 15;
}

// Destructor.
Sat::~Sat() {
  // We need to have called Cleanup() at this point.
  // We should probably enforce this.
}

#define ARG_KVALUE(argument, variable, value) \
  if (!strcmp(argv[i], argument)) {           \
    variable = value;                         \
    continue;                                 \
  }

#define ARG_IVALUE(argument, variable)                   \
  if (!strcmp(argv[i], argument)) {                      \
    i++;                                                 \
    if (i < argc) variable = strtoull(argv[i], NULL, 0); \
    continue;                                            \
  }

#define ARG_SVALUE(argument, variable)                                 \
  if (!strcmp(argv[i], argument)) {                                    \
    i++;                                                               \
    if (i < argc) snprintf(variable, sizeof(variable), "%s", argv[i]); \
    continue;                                                          \
  }

// Configures SAT from command line arguments.
// This will call exit() given a request for
// self-documentation or unexpected args.
bool Sat::ParseArgs(int argc, const char **argv) {
  int i;
  uint64 filesize = page_length_ * disk_pages_;

  // Parse each argument.
  for (i = 1; i < argc; i++) {
    // Switch to fall back to corase-grain-lock queue. (for benchmarking)
    ARG_KVALUE("--coarse_grain_lock", pe_q_implementation_, SAT_ONELOCK);

    // Set number of megabyte to use.
    ARG_IVALUE("-M", size_mb_);

    // Specify the amount of megabytes to be reserved for system.
    ARG_IVALUE("--reserve_memory", reserve_mb_);

    // Set minimum megabytes of hugepages to require.
    ARG_IVALUE("-H", min_hugepages_mbytes_);

    // Set number of seconds to run.
    ARG_IVALUE("-s", runtime_seconds_);

    // Set number of memory copy threads.
    ARG_IVALUE("-m", memory_threads_);

    // Set number of memory invert threads.
    ARG_IVALUE("-i", invert_threads_);

    // Set number of check-only threads.
    ARG_IVALUE("-c", check_threads_);

    // Set number of cache line size datastructures.
    ARG_IVALUE("--cc_inc_count", cc_inc_count_);

    // Set number of cache line size datastructures
    ARG_IVALUE("--cc_line_count", cc_cacheline_count_);

    // Override the detected or assumed cache line size.
    ARG_IVALUE("--cc_line_size", cc_cacheline_size_);

    // Flag set when cache coherency tests need to be run
    ARG_KVALUE("--cc_test", cc_test_, true);

    // Set when the cpu_frequency test needs to be run
    ARG_KVALUE("--cpu_freq_test", cpu_freq_test_, true);

    // Set the threshold in MHz at which the cpu frequency test will fail.
    ARG_IVALUE("--cpu_freq_threshold", cpu_freq_threshold_);

    // Set the rounding value for the cpu frequency test. The default is to
    // round to the nearest 10s value.
    ARG_IVALUE("--cpu_freq_round", cpu_freq_round_);

    // Set number of CPU stress threads.
    ARG_IVALUE("-C", cpu_stress_threads_);

    // Set logfile name.
    ARG_SVALUE("-l", logfilename_);

    // Verbosity level.
    ARG_IVALUE("-v", verbosity_);

    // Chatty printout level.
    ARG_IVALUE("--printsec", print_delay_);

    // Turn off timestamps logging.
    ARG_KVALUE("--no_timestamps", log_timestamps_, false);

    // Set maximum number of errors to collect. Stop running after this many.
    ARG_IVALUE("--max_errors", max_errorcount_);

    // Set pattern block size.
    ARG_IVALUE("-p", page_length_);

    // Set pattern block size.
    ARG_IVALUE("--filesize", filesize);

    // NUMA options.
    ARG_KVALUE("--no_affinity", use_affinity_, false);
    ARG_KVALUE("--local_numa", region_mode_, kLocalNuma);
    ARG_KVALUE("--remote_numa", region_mode_, kRemoteNuma);

    // Inject errors to force miscompare code paths
    ARG_KVALUE("--force_errors", error_injection_, true);
    ARG_KVALUE("--force_errors_like_crazy", crazy_error_injection_, true);
    if (crazy_error_injection_) error_injection_ = true;

    // Stop immediately on any arror, for debugging HW problems.
    ARG_KVALUE("--stop_on_errors", stop_on_error_, 1);

    // Never check data as you go.
    ARG_KVALUE("-F", strict_, 0);

    // Warm the cpu as you go.
    ARG_KVALUE("-W", warm_, 1);

    // Allow runnign on unknown systems with base unimplemented OsLayer
    ARG_KVALUE("-A", run_on_anything_, 1);

    // Size of read blocks for disk test.
    ARG_IVALUE("--read-block-size", read_block_size_);

    // Size of write blocks for disk test.
    ARG_IVALUE("--write-block-size", write_block_size_);

    // Size of segment for disk test.
    ARG_IVALUE("--segment-size", segment_size_);

    // Size of disk cache size for disk test.
    ARG_IVALUE("--cache-size", cache_size_);

    // Number of blocks to test per segment.
    ARG_IVALUE("--blocks-per-segment", blocks_per_segment_);

    // Maximum time a block read should take before warning.
    ARG_IVALUE("--read-threshold", read_threshold_);

    // Maximum time a block write should take before warning.
    ARG_IVALUE("--write-threshold", write_threshold_);

    // Do not write anything to disk in the disk test.
    ARG_KVALUE("--destructive", non_destructive_, 0);

    // Run SAT in monitor mode. No test load at all.
    ARG_KVALUE("--monitor_mode", monitor_mode_, true);

    // Run SAT in address mode. Tag all cachelines by virt addr.
    ARG_KVALUE("--tag_mode", tag_mode_, true);

    // Dump range map of tested pages..
    ARG_KVALUE("--do_page_map", do_page_map_, true);

    // Specify the physical address base to test.
    ARG_IVALUE("--paddr_base", paddr_base_);

    // Specify the frequency for power spikes.
    ARG_IVALUE("--pause_delay", pause_delay_);

    // Specify the duration of each pause (for power spikes).
    ARG_IVALUE("--pause_duration", pause_duration_);

    // Disk device names
    if (!strcmp(argv[i], "-d")) {
      i++;
      if (i < argc) {
        disk_threads_++;
        diskfilename_.push_back(string(argv[i]));
        blocktables_.push_back(new DiskBlockTable());
      }
      continue;
    }

    // Set number of disk random threads for each disk write thread.
    ARG_IVALUE("--random-threads", random_threads_);

    // Set a tempfile to use in a file thread.
    if (!strcmp(argv[i], "-f")) {
      i++;
      if (i < argc) {
        file_threads_++;
        filename_.push_back(string(argv[i]));
      }
      continue;
    }

    // Set a hostname to use in a network thread.
    if (!strcmp(argv[i], "-n")) {
      i++;
      if (i < argc) {
        net_threads_++;
        ipaddrs_.push_back(string(argv[i]));
      }
      continue;
    }

    // Run threads that listen for incoming SAT net connections.
    ARG_KVALUE("--listen", listen_threads_, 1);

    ARG_IVALUE("--channel_hash", channel_hash_);
    ARG_IVALUE("--channel_width", channel_width_);

    if (!strcmp(argv[i], "--memory_channel")) {
      i++;
      if (i < argc) {
        const char *channel = argv[i];
        channels_.push_back(vector<string>());
        while (const char *next = strchr(channel, ',')) {
          channels_.back().push_back(string(channel, next - channel));
          channel = next + 1;
        }
        channels_.back().push_back(string(channel));
      }
      continue;
    }

    // Set disk_pages_ if filesize or page size changed.
    if (filesize !=
        static_cast<uint64>(page_length_) * static_cast<uint64>(disk_pages_)) {
      disk_pages_ = filesize / page_length_;
      if (disk_pages_ == 0) disk_pages_ = 1;
    }

    // Default:
    PrintHelp();
    if (strcmp(argv[i], "-h") && strcmp(argv[i], "--help")) {
      printf("\n Unknown argument %s\n", argv[i]);
      exit(1);
    }
    // Forget it, we printed the help, just bail.
    // We don't want to print test status, or any log parser stuff.
    exit(0);
  }

  Logger::GlobalLogger()->SetVerbosity(verbosity_);

  // Update relevant data members with parsed input.
  // Translate MB into bytes.
  size_ = static_cast<int64>(size_mb_) * kMegabyte;

  // Set logfile flag.
  if (strcmp(logfilename_, "")) use_logfile_ = true;

  cmdline_ = ocpdiag::results::CommandLineStringFromMainArgs(argc, argv);
  cmdline_json_ = ocpdiag::results::ParameterJsonFromMainArgs(argc, argv);

  return true;
}

bool Sat::ValidateArgs() {
  // Checks valid page length.
  if (page_length_ && !(page_length_ & (page_length_ - 1)) &&
      (page_length_ > 1023)) {
    // Prints if we have changed from default.
    if (page_length_ != kSatPageSize) {
      test_run_->AddPreStartLog(Log{
          .severity = LogSeverity::kDebug,
          .message = absl::StrFormat("Updating page size to %d", page_length_),
      });
    }
  } else {
    // Revert to default page length.
    test_run_->AddPreStartError(Error{
        .symptom = kProcessError,
        .message = absl::StrFormat("Invalid page size %d", page_length_),
    });
    page_length_ = kSatPageSize;
    return false;
  }

  // Validate memory channel parameters if supplied
  if (channels_.size()) {
    if (channels_.size() == 1) {
      channel_hash_ = 0;
      test_run_->AddPreStartLog(Log{
          .severity = LogSeverity::kInfo,
          .message =
              "Only one memory channel...deactivating interleave decoding",
      });
    } else if (channels_.size() > 2) {
      test_run_->AddPreStartError(Error{
          .symptom = kProcessError,
          .message = "Triple-channel mode not yet supported",
      });
      return false;
    }
    for (uint i = 0; i < channels_.size(); i++)
      if (channels_[i].size() != channels_[0].size()) {
        test_run_->AddPreStartError(Error{
            .symptom = kProcessError,
            .message = absl::StrFormat(
                "Channels 0 and %d have a different count of dram modules", i),
        });
        return false;
      }
    if (channels_[0].size() & (channels_[0].size() - 1)) {
      test_run_->AddPreStartError(Error{
          .symptom = kProcessError,
          .message = "Amount of modules per memory channel is not a power of 2",
      });
      return false;
    }
    if (channel_width_ < 16 || channel_width_ & (channel_width_ - 1)) {
      test_run_->AddPreStartError(Error{
          .symptom = kProcessError,
          .message =
              absl::StrFormat("Channel width %d is invalid.\n", channel_width_),
      });
      return false;
    }
    if (channel_width_ / channels_[0].size() < 8) {
      test_run_->AddPreStartError(Error{
          .symptom = kProcessError,
          .message = absl::StrFormat("Chip width x%d must be x8 or greater",
                                     channel_width_ / channels_[0].size()),
      });
      return false;
    }
  }

  return true;
}

void Sat::PrintHelp() {
  printf(
      "Usage: ./sat(32|64) [options]\n"
      " -M mbytes        megabytes of ram to test\n"
      " --reserve-memory If not using hugepages, the amount of memory to "
      " reserve for the system\n"
      " -H mbytes        minimum megabytes of hugepages to require\n"
      " -s seconds       number of seconds to run\n"
      " -m threads       number of memory copy threads to run\n"
      " -i threads       number of memory invert threads to run\n"
      " -C threads       number of memory CPU stress threads to run\n"
      " -d device        add a direct write disk thread with block "
      "device (or file) 'device'\n"
      " -f filename      add a disk thread with "
      "tempfile 'filename'\n"
      " -l logfile       log output to file 'logfile'\n"
      " --no_timestamps  do not prefix timestamps to log messages\n"
      " --max_errors n   exit early after finding 'n' errors\n"
      " -v level         verbosity (0-20), default is 8\n"
      " --printsec secs  How often to print 'seconds remaining'\n"
      " -W               Use more CPU-stressful memory copy\n"
      " -A               run in degraded mode on incompatible systems\n"
      " -p pagesize      size in bytes of memory chunks\n"
      " --filesize size  size of disk IO tempfiles\n"
      " -n ipaddr        add a network thread connecting to "
      "system at 'ipaddr'\n"
      " --listen         run a thread to listen for and respond "
      "to network threads.\n"
      " --force_errors   inject false errors to test error handling\n"
      " --force_errors_like_crazy   inject a lot of false errors "
      "to test error handling\n"
      " -F               don't result check each transaction\n"
      " --stop_on_errors  Stop after finding the first error.\n"
      " --read-block-size     size of block for reading (-d)\n"
      " --write-block-size    size of block for writing (-d). If not "
      "defined, the size of block for writing will be defined as the "
      "size of block for reading\n"
      " --segment-size   size of segments to split disk into (-d)\n"
      " --cache-size     size of disk cache (-d)\n"
      " --blocks-per-segment  number of blocks to read/write per "
      "segment per iteration (-d)\n"
      " --read-threshold      maximum time (in us) a block read should "
      "take (-d)\n"
      " --write-threshold     maximum time (in us) a block write "
      "should take (-d)\n"
      " --random-threads      number of random threads for each disk "
      "write thread (-d)\n"
      " --destructive    write/wipe disk partition (-d)\n"
      " --monitor_mode   only do ECC error polling, no stress load.\n"
      " --cc_test        do the cache coherency testing\n"
      " --cc_inc_count   number of times to increment the "
      "cacheline's member\n"
      " --cc_line_count  number of cache line sized datastructures "
      "to allocate for the cache coherency threads to operate\n"
      " --cc_line_size   override the auto-detected cache line size\n"
      " --cpu_freq_test  enable the cpu frequency test (requires the "
      "--cpu_freq_threshold argument to be set)\n"
      " --cpu_freq_threshold  fail the cpu frequency test if the frequency "
      "goes below this value (specified in MHz)\n"
      " --cpu_freq_round round the computed frequency to this value, if set"
      " to zero, only round to the nearest MHz\n"
      " --paddr_base     allocate memory starting from this address\n"
      " --pause_delay    delay (in seconds) between power spikes\n"
      " --pause_duration duration (in seconds) of each pause\n"
      " --no_affinity    do not set any cpu affinity\n"
      " --local_numa     choose memory regions associated with "
      "each CPU to be tested by that CPU\n"
      " --remote_numa    choose memory regions not associated with "
      "each CPU to be tested by that CPU\n"
      " --channel_hash   mask of address bits XORed to determine channel. "
      "Mask 0x40 interleaves cachelines between channels\n"
      " --channel_width bits     width in bits of each memory channel\n"
      " --memory_channel u1,u2   defines a comma-separated list of names "
      "for dram packages in a memory channel. Use multiple times to "
      "define multiple channels.\n");
}

// Launch the SAT task threads. Returns 0 on error.
void Sat::InitializeThreads(TestStep &test_step) {
  // Skip creating threads if in monitor mode.
  if (monitor_mode_) return;

  AcquireWorkerLock();

  test_step.AddLog(
      Log{.severity = LogSeverity::kDebug, "Starting worker threads"});

  WorkerVector *memory_vector = new WorkerVector();
  std::unique_ptr<TestStep> copy_step;
  if (memory_threads_ > 0) {
    copy_step =
        std::make_unique<TestStep>("Run Memory Copy Threads", *test_run_);
  }
  for (int i = 0; i < memory_threads_; i++) {
    CopyThread *thread = new CopyThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &power_spike_status_, copy_step.get());

    if ((region_count_ > 1) && (region_mode_)) {
      int32 region = region_find(i % region_count_);
      cpu_set_t *cpuset = os_->FindCoreMask(region, *copy_step);
      sat_assert(cpuset);
      if (region_mode_ == kLocalNuma) {
        // Choose regions associated with this CPU.
        thread->set_cpu_mask(cpuset);
        thread->set_tag(1 << region);
      } else if (region_mode_ == kRemoteNuma) {
        // Choose regions not associated with this CPU..
        thread->set_cpu_mask(cpuset);
        thread->set_tag(region_mask_ & ~(1 << region));
      }
    } else {
      cpu_set_t available_cpus;
      thread->AvailableCpus(&available_cpus);
      int cores = cpuset_count(&available_cpus);
      // Don't restrict thread location if we have more than one
      // thread per core. Not so good for performance.
      if (cpu_stress_threads_ + memory_threads_ <= cores) {
        // Place a thread on alternating cores first.
        // This assures interleaved core use with no overlap.
        int nthcore = i;
        int nthbit =
            (((2 * nthcore) % cores) + (((2 * nthcore) / cores) % 2)) % cores;
        cpu_set_t all_cores;
        cpuset_set_ab(&all_cores, 0, cores);
        if (!cpuset_isequal(&available_cpus, &all_cores)) {
          // We are assuming the bits are contiguous.
          // Complain if this is not so.
          copy_step->AddLog(Log{
              .severity = LogSeverity::kWarning,
              .message = absl::StrFormat("Did not find the expected number of "
                                         "CPU cores. Expected: %s, Actual: %s",
                                         cpuset_format(&all_cores),
                                         cpuset_format(&available_cpus))});
        }

        // Set thread affinity.
        thread->set_cpu_mask_to_cpu(nthbit);
      }
    }
    memory_vector->insert(memory_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kMemoryType, memory_vector));
  if (memory_threads_ > 0) thread_test_steps_.push_back(std::move(copy_step));

  // File IO threads.
  std::unique_ptr<TestStep> file_io_step;
  if (file_threads_ > 0) {
    file_io_step =
        std::make_unique<TestStep>("Run File IO Threads", *test_run_);
  }
  WorkerVector *fileio_vector = new WorkerVector();
  for (int i = 0; i < file_threads_; i++) {
    FileThread *thread = new FileThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &power_spike_status_, file_io_step.get());
    thread->SetFile(filename_[i]);
    // Set disk threads high priority. They don't take much processor time,
    // but blocking them will delay disk IO.
    thread->SetPriority(WorkerThread::High);

    fileio_vector->insert(fileio_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kFileIOType, fileio_vector));
  if (file_threads_ > 0) thread_test_steps_.push_back(std::move(file_io_step));

  // Net IO threads.
  WorkerVector *netslave_vector = new WorkerVector();
  if (listen_threads_ > 0) {
    auto net_listen_step = std::make_unique<TestStep>(
        "Listen for Incoming Network IO", *test_run_);
    // Create a network slave thread. This listens for connections.
    NetworkListenThread *thread = new NetworkListenThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_, net_listen_step.get());

    thread_test_steps_.push_back(std::move(net_listen_step));
    netslave_vector->insert(netslave_vector->end(), thread);
  }

  std::unique_ptr<TestStep> net_io_step;
  if (net_threads_ > 0) {
    net_io_step =
        std::make_unique<TestStep>("Run Network IO Threads", *test_run_);
  }
  WorkerVector *netio_vector = new WorkerVector();
  for (int i = 0; i < net_threads_; i++) {
    NetworkThread *thread = new NetworkThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_, net_io_step.get());
    thread->SetIP(ipaddrs_[i].c_str());

    netio_vector->insert(netio_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kNetIOType, netio_vector));
  workers_map_.insert(make_pair(kNetSlaveType, netslave_vector));
  if (net_threads_ > 0) thread_test_steps_.push_back(std::move(net_io_step));

  // Result check threads.
  std::unique_ptr<TestStep> check_step;
  if (check_threads_ > 0) {
    check_step = std::make_unique<TestStep>("Run Mid-Test Memory Check Threads",
                                            *test_run_);
  }
  WorkerVector *check_vector = new WorkerVector();
  for (int i = 0; i < check_threads_; i++) {
    CheckThread *thread = new CheckThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_, check_step.get());

    check_vector->insert(check_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kCheckType, check_vector));
  if (check_threads_ > 0) thread_test_steps_.push_back(std::move(check_step));

  // Memory invert threads.
  std::unique_ptr<TestStep> invert_step;
  if (invert_threads_ > 0) {
    invert_step =
        std::make_unique<TestStep>("Run Memory Invert Threads", *test_run_);
    invert_step->AddLog(Log{.severity = LogSeverity::kDebug,
                            .message = "Starting memory invert threads"});
  }
  WorkerVector *invert_vector = new WorkerVector();
  for (int i = 0; i < invert_threads_; i++) {
    InvertThread *thread = new InvertThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_, invert_step.get());

    invert_vector->insert(invert_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kInvertType, invert_vector));
  if (invert_threads_ > 0) thread_test_steps_.push_back(std::move(invert_step));

  // Disk stress threads.
  std::unique_ptr<TestStep> disk_step;
  if (disk_threads_ > 0) {
    disk_step =
        std::make_unique<TestStep>("Run Disk Stress Threads", *test_run_);
    disk_step->AddLog(Log{
        .severity = LogSeverity::kDebug,
        .message = "Starting disk stress threads",
    });
  }
  WorkerVector *disk_vector = new WorkerVector();
  WorkerVector *random_vector = new WorkerVector();
  for (int i = 0; i < disk_threads_; i++) {
    // Creating write threads
    DiskThread *thread = new DiskThread(blocktables_[i]);
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &power_spike_status_, disk_step.get());
    thread->SetDevice(diskfilename_[i].c_str());
    if (thread->SetParameters(read_block_size_, write_block_size_,
                              segment_size_, cache_size_, blocks_per_segment_,
                              read_threshold_, write_threshold_,
                              non_destructive_)) {
      disk_vector->insert(disk_vector->end(), thread);
    } else {
      disk_step->AddLog(Log{
          .severity = LogSeverity::kDebug,
          .message = "Failed to set disk thread parameters",
      });
      delete thread;
    }

    for (int j = 0; j < random_threads_; j++) {
      // Creating random threads
      RandomDiskThread *rthread = new RandomDiskThread(blocktables_[i]);
      rthread->InitThread(total_threads_++, this, os_, patternlist_,
                          &power_spike_status_, disk_step.get());
      rthread->SetDevice(diskfilename_[i].c_str());
      if (rthread->SetParameters(read_block_size_, write_block_size_,
                                 segment_size_, cache_size_,
                                 blocks_per_segment_, read_threshold_,
                                 write_threshold_, non_destructive_)) {
        random_vector->insert(random_vector->end(), rthread);
      } else {
        disk_step->AddLog(Log{
            .severity = LogSeverity::kDebug,
            .message = "Failed to set random disk thread parameters",
        });
        delete rthread;
      }
    }
  }

  workers_map_.insert(make_pair(kDiskType, disk_vector));
  workers_map_.insert(make_pair(kRandomDiskType, random_vector));
  if (disk_threads_ > 0) thread_test_steps_.push_back(std::move(disk_step));

  // CPU stress threads.
  std::unique_ptr<TestStep> cpu_stress_step;
  if (cpu_stress_threads_ > 0) {
    cpu_stress_step =
        std::make_unique<TestStep>("Run CPU Stress Threads", *test_run_);
    cpu_stress_step->AddLog(Log{
        .severity = LogSeverity::kDebug,
        .message = "Starting cpu stress threads",
    });
  }
  WorkerVector *cpu_vector = new WorkerVector();
  for (int i = 0; i < cpu_stress_threads_; i++) {
    CpuStressThread *thread = new CpuStressThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_, cpu_stress_step.get());

    // Don't restrict thread location if we have more than one
    // thread per core. Not so good for performance.
    cpu_set_t available_cpus;
    thread->AvailableCpus(&available_cpus);
    int cores = cpuset_count(&available_cpus);
    if (cpu_stress_threads_ + memory_threads_ <= cores) {
      // Place a thread on alternating cores first.
      // Go in reverse order for CPU stress threads. This assures interleaved
      // core use with no overlap.
      int nthcore = (cores - 1) - i;
      int nthbit =
          (((2 * nthcore) % cores) + (((2 * nthcore) / cores) % 2)) % cores;
      cpu_set_t all_cores;
      cpuset_set_ab(&all_cores, 0, cores);
      if (!cpuset_isequal(&available_cpus, &all_cores)) {
        cpu_stress_step->AddLog(Log{
            .severity = LogSeverity::kWarning,
            .message = absl::StrFormat("Found %s cores when %s were expected",
                                       cpuset_format(&available_cpus),
                                       cpuset_format(&all_cores))});
      }

      // Set thread affinity.
      thread->set_cpu_mask_to_cpu(nthbit);
    }

    cpu_vector->insert(cpu_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kCPUType, cpu_vector));
  if (cpu_stress_threads_ > 0)
    thread_test_steps_.push_back(std::move(cpu_stress_step));

  // CPU Cache Coherency Threads - one for each core available.
  if (cc_test_) {
    auto cpu_cache_step =
        std::make_unique<TestStep>("Run CPU Cache Coherency Test", *test_run_);
    WorkerVector *cc_vector = new WorkerVector();
    cpu_cache_step->AddLog(Log{
        .severity = LogSeverity::kDebug,
        .message = "Starting cpu cache coherency threads",
    });

    // Allocate the shared datastructure to be worked on by the threads.
    cc_cacheline_data_ = reinterpret_cast<cc_cacheline_data *>(
        malloc(sizeof(cc_cacheline_data) * cc_cacheline_count_));
    sat_assert(cc_cacheline_data_ != NULL);

    // Initialize the strucutre.
    memset(cc_cacheline_data_, 0,
           sizeof(cc_cacheline_data) * cc_cacheline_count_);

    int num_cpus = CpuCount();
    char *num;
    // Calculate the number of cache lines needed just to give each core
    // its own counter.
    int line_size = cc_cacheline_size_;
    if (line_size <= 0) {
      line_size = CacheLineSize();
      if (line_size < kCacheLineSize) line_size = kCacheLineSize;
    }
    cpu_cache_step->AddMeasurement(Measurement{
        .name = "Cache Line Size",
        .unit = "bytes",
        .value = static_cast<double>(line_size),
    });
    // The number of cache lines needed to hold an array of num_cpus.
    // "num" must be the same type as cc_cacheline_data[X].num or the memory
    // size calculations will fail.
    int needed_lines = (sizeof(*num) * num_cpus + line_size - 1) / line_size;
    // Allocate all the nums once so that we get a single chunk
    // of contiguous memory.
#ifdef HAVE_POSIX_MEMALIGN
    int err_result =
        posix_memalign(reinterpret_cast<void **>(&num), line_size,
                       line_size * needed_lines * cc_cacheline_count_);
#else
    num = reinterpret_cast<int *>(
        memalign(line_size, line_size * needed_lines * cc_cacheline_count_));
    int err_result = (num == 0);
#endif
    sat_assert(err_result == 0);

    int cline;
    for (cline = 0; cline < cc_cacheline_count_; cline++) {
      memset(num, 0, sizeof(*num) * num_cpus);
      cc_cacheline_data_[cline].num = num;
      num += (line_size * needed_lines) / sizeof(*num);
    }

    int tnum;
    for (tnum = 0; tnum < num_cpus; tnum++) {
      CpuCacheCoherencyThread *thread =
          new CpuCacheCoherencyThread(cc_cacheline_data_, cc_cacheline_count_,
                                      tnum, num_cpus, cc_inc_count_);
      thread->InitThread(total_threads_++, this, os_, patternlist_,
                         &continuous_status_, cpu_cache_step.get());
      // Pin the thread to a particular core.
      thread->set_cpu_mask_to_cpu(tnum);

      // Insert the thread into the vector.
      cc_vector->insert(cc_vector->end(), thread);
    }
    workers_map_.insert(make_pair(kCCType, cc_vector));
    thread_test_steps_.push_back(std::move(cpu_cache_step));
  }

  if (cpu_freq_test_) {
    auto cpu_freq_step =
        std::make_unique<TestStep>("Run CPU Frequency Test", *test_run_);
    // Create the frequency test thread.
    cpu_freq_step->AddLog(Log{
        .severity = LogSeverity::kDebug,
        .message = "Running CPU frequency test.",
    });
    CpuFreqThread *thread =
        new CpuFreqThread(CpuCount(), cpu_freq_threshold_, cpu_freq_round_);
    // This thread should be paused when other threads are paused.
    thread->InitThread(total_threads_++, this, os_, NULL, &power_spike_status_,
                       cpu_freq_step.get());

    WorkerVector *cpu_freq_vector = new WorkerVector();
    cpu_freq_vector->insert(cpu_freq_vector->end(), thread);
    workers_map_.insert(make_pair(kCPUFreqType, cpu_freq_vector));
    thread_test_steps_.push_back(std::move(cpu_freq_step));
  }

  ReleaseWorkerLock();
}

// Return the number of cpus actually present in the machine.
int Sat::CpuCount() { return sysconf(_SC_NPROCESSORS_CONF); }

int Sat::ReadInt(const char *filename, int *value) {
  char line[64];
  int fd = open(filename, O_RDONLY), err = -1;

  if (fd < 0) return -1;
  if (read(fd, line, sizeof(line)) > 0) {
    *value = atoi(line);
    err = 0;
  }

  close(fd);
  return err;
}

// Return the worst case (largest) cache line size of the various levels of
// cache actually prsent in the machine.
int Sat::CacheLineSize() {
  int max_linesize, linesize;
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
  max_linesize = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
          &max_linesize);
#endif
#ifdef _SC_LEVEL2_DCACHE_LINESIZE
  linesize = sysconf(_SC_LEVEL2_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index1/coherency_line_size",
          &linesize);
#endif
  if (linesize > max_linesize) max_linesize = linesize;
#ifdef _SC_LEVEL3_DCACHE_LINESIZE
  linesize = sysconf(_SC_LEVEL3_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index2/coherency_line_size",
          &linesize);
#endif
  if (linesize > max_linesize) max_linesize = linesize;
#ifdef _SC_LEVEL4_DCACHE_LINESIZE
  linesize = sysconf(_SC_LEVEL4_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index3/coherency_line_size",
          &linesize);
#endif
  if (linesize > max_linesize) max_linesize = linesize;
  return max_linesize;
}

// Notify and reap worker threads.
void Sat::JoinThreads(TestStep &test_step) {
  test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                       .message = "Joining worker threads"});
  power_spike_status_.StopWorkers();
  continuous_status_.StopWorkers();

  AcquireWorkerLock();
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      test_step.AddLog(Log{
          .severity = LogSeverity::kDebug,
          .message = absl::StrFormat("Joining thread %d", (*it)->ThreadID())});
      (*it)->JoinThread();
    }
  }
  ReleaseWorkerLock();

  QueueStats(test_step);

  {
    TestStep check_step("Run Post-Test Memory Check Threads", *test_run_);

    // Finish up result checking.
    // Spawn check threads to minimize check time.
    check_step.AddLog(
        Log{.severity = LogSeverity::kDebug,
            .message = "Finished countdown, beginning to check results"});
    WorkerStatus reap_check_status;
    WorkerVector reap_check_vector;

    // No need for check threads for monitor mode.
    if (!monitor_mode_) {
      // Initialize the check threads.
      for (int i = 0; i < fill_threads_; i++) {
        CheckThread *thread = new CheckThread();
        thread->InitThread(total_threads_++, this, os_, patternlist_,
                           &reap_check_status, &check_step);
        reap_check_vector.push_back(thread);
      }
    }

    reap_check_status.Initialize();
    // Check threads should be marked to stop ASAP.
    reap_check_status.StopWorkers();

    // Spawn the check threads.
    for (WorkerVector::const_iterator it = reap_check_vector.begin();
         it != reap_check_vector.end(); ++it) {
      check_step.AddLog(Log{
          .severity = LogSeverity::kDebug,
          .message =
              absl::StrFormat("Spawning check thread %d", (*it)->ThreadID())});
      (*it)->SpawnThread();
    }

    // Join the check threads.
    for (WorkerVector::const_iterator it = reap_check_vector.begin();
         it != reap_check_vector.end(); ++it) {
      check_step.AddLog(Log{.severity = LogSeverity::kDebug,
                            .message = absl::StrFormat(
                                "Joining check thread %d", (*it)->ThreadID())});
      (*it)->JoinThread();
    }

    // Add in any errors from check threads.
    for (WorkerVector::const_iterator it = reap_check_vector.begin();
         it != reap_check_vector.end(); ++it) {
      check_step.AddLog(Log{
          .severity = LogSeverity::kDebug,
          .message = absl::StrFormat("Reaping thread %d", (*it)->ThreadID())});
      errorcount_ += (*it)->GetErrorCount();
      check_step.AddLog(Log{.severity = LogSeverity::kDebug,
                            .message = absl::StrFormat(
                                "Thread %d found %lld hardware incidents",
                                (*it)->ThreadID(), (*it)->GetErrorCount())});
      delete (*it);
    }
    reap_check_vector.clear();
    reap_check_status.Destroy();
  }

  // Reap all children. Stopped threads should have already ended.
  // Result checking threads will end when they have finished
  // result checking.
  test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                       .message = "Join all outstanding threads"});

  // Find all errors.
  errorcount_ = GetTotalErrorCount();

  AcquireWorkerLock();
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                           .message = absl::StrFormat(
                               "Reaping thread status %d", (*it)->ThreadID())});
      test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                           .message = absl::StrFormat(
                               "Thread %d found %lld hardware incidents",
                               (*it)->ThreadID(), (*it)->GetErrorCount())});
    }
  }
  ReleaseWorkerLock();

  // Delete thread test steps
  for (std::unique_ptr<TestStep> &thread_step : thread_test_steps_)
    thread_step.reset();
}

// Print queuing information.
void Sat::QueueStats(TestStep &test_step) {
  finelock_q_->QueueAnalysis(test_step);
}

void Sat::AnalysisAllStats(TestStep &test_step) {
  double max_runtime_sec = 0.;
  double total_data = 0.;
  double total_bandwidth = 0.;
  double thread_runtime_sec = 0.;

  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      thread_runtime_sec = (*it)->GetRunDurationUSec() * 1.0 / 1000000.;
      total_data += (*it)->GetMemoryCopiedData();
      total_data += (*it)->GetDeviceCopiedData();
      if (thread_runtime_sec > max_runtime_sec) {
        max_runtime_sec = thread_runtime_sec;
      }
    }
  }

  total_bandwidth = total_data / max_runtime_sec;

  test_step.AddMeasurement(Measurement{
      .name = "Total Data Copied",
      .unit = "MB",
      .value = total_data,
  });
  test_step.AddMeasurement(Measurement{
      .name = "Run Time",
      .unit = "s",
      .value = max_runtime_sec,
  });
  test_step.AddMeasurement(Measurement{
      .name = "Total Bandwidth",
      .unit = "MB/s",
      .value = total_bandwidth,
  });
  test_step.AddMeasurement(Measurement{
      .name = "Total Hardware Incidents",
      .validators = {Validator{.type = ValidatorType::kEqual, .value = {0.}}},
      .value = static_cast<double>(errorcount_),
  });
}

void Sat::ReportThreadStats(vector<ThreadType> thread_types,
                            string measurement_name, bool use_device_data,
                            TestStep &test_step) {
  double data = 0;
  double bandwidth = 0;
  for (ThreadType thread_type : thread_types) {
    WorkerMap::const_iterator outer_it =
        workers_map_.find(static_cast<int>(thread_type));
    sat_assert(outer_it != workers_map_.end());
    for (WorkerVector::const_iterator inner_it = outer_it->second->begin();
         inner_it != outer_it->second->end(); ++inner_it) {
      if (use_device_data) {
        data += (*inner_it)->GetDeviceCopiedData();
        bandwidth += (*inner_it)->GetDeviceBandwidth();
      } else {
        data += (*inner_it)->GetMemoryCopiedData();
        bandwidth += (*inner_it)->GetMemoryBandwidth();
      }
    }
  }

  test_step.AddMeasurement(Measurement{
      .name = absl::StrCat(measurement_name, " Data Copied"),
      .unit = "MB",
      .value = data,
  });
  test_step.AddMeasurement(Measurement{
      .name = absl::StrCat(measurement_name, " Bandwidth"),
      .unit = "MB/s",
      .value = bandwidth,
  });
}

// Process worker thread data for bandwidth information, and error results.
// You can add more methods here just subclassing SAT.
void Sat::RunAnalysis() {
  TestStep analysis_step("Run and Report Thread Analysis", *test_run_);
  AnalysisAllStats(analysis_step);
  if (memory_threads_ > 0)
    ReportThreadStats({kMemoryType, kFileIOType}, "Memory", false,
                      analysis_step);
  if (file_threads_ > 0)
    ReportThreadStats({kFileIOType}, "File", true, analysis_step);
  if (check_threads_ > 0)
    ReportThreadStats({kCheckType}, "Check", false, analysis_step);
  if (net_threads_ > 0)
    ReportThreadStats({kNetIOType, kNetSlaveType}, "Net", true, analysis_step);
  if (invert_threads_ > 0)
    ReportThreadStats({kInvertType}, "Invert", false, analysis_step);
  if (disk_threads_ > 0)
    ReportThreadStats({kDiskType, kRandomDiskType}, "Disk", true,
                      analysis_step);
}

// Get total error count, summing across all threads..
int64 Sat::GetTotalErrorCount() {
  int64 errors = 0;

  AcquireWorkerLock();
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      errors += (*it)->GetErrorCount();
    }
  }
  ReleaseWorkerLock();
  return errors;
}

void Sat::SpawnThreads(TestStep &test_step) {
  test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                       .message = "Initializing WorkerStatus objects"});
  power_spike_status_.Initialize();
  continuous_status_.Initialize();
  test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                       .message = "Spawning worker threads"});
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      test_step.AddLog(Log{
          .severity = LogSeverity::kDebug,
          .message =
              absl::StrFormat("Spawning worker thread %d", (*it)->ThreadID())});
      (*it)->SpawnThread();
    }
  }
}

// Delete used worker thread objects.
void Sat::DeleteThreads(TestStep &test_step) {
  test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                       .message = "Deleting worker threads"});
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      test_step.AddLog(Log{
          .severity = LogSeverity::kDebug,
          .message = absl::StrFormat("Deleting thread %d", (*it)->ThreadID())});
      delete (*it);
    }
    delete map_it->second;
  }
  workers_map_.clear();
  test_step.AddLog(Log{.severity = LogSeverity::kDebug,
                       .message = "Destroying WorkerStatus objects"});
  power_spike_status_.Destroy();
  continuous_status_.Destroy();
}

namespace {
// Calculates the next time an action in Sat::Run() should occur, based on a
// schedule derived from a start point and a regular frequency.
//
// Using frequencies instead of intervals with their accompanying drift
// allows users to better predict when the actions will occur throughout a
// run.
//
// Arguments:
//   frequency: seconds
//   start: unixtime
//   now: unixtime
//
// Returns: unixtime
inline time_t NextOccurance(time_t frequency, time_t start, time_t now) {
  return start + frequency + (((now - start) / frequency) * frequency);
}
}  // namespace

// Run the actual test.
bool Sat::Run() {
  // Install signal handlers to gracefully exit in the middle of a run.
  //
  // Why go through this whole rigmarole?  It's the only standards-compliant
  // (C++ and POSIX) way to handle signals in a multithreaded program.
  // Specifically:
  //
  // 1) (C++) The value of a variable not of type "volatile sig_atomic_t" is
  //    unspecified upon entering a signal handler and, if modified by the
  //    handler, is unspecified after leaving the handler.
  //
  // 2) (POSIX) After the value of a variable is changed in one thread,
  // another
  //    thread is only guaranteed to see the new value after both threads
  //    have acquired or released the same mutex or rwlock, synchronized to
  //    the same barrier, or similar.
  //
  // #1 prevents the use of #2 in a signal handler, so the signal handler
  // must be called in the same thread that reads the "volatile sig_atomic_t"
  // variable it sets.  We enforce that by blocking the signals in question
  // in the worker threads, forcing them to be handled by this thread.
  TestStep run_step("Run Test Threads", *test_run_);
  run_step.AddLog(Log{.severity = LogSeverity::kDebug,
                      .message = "Installing signal handlers"});
  sigset_t new_blocked_signals;
  sigemptyset(&new_blocked_signals);
  sigaddset(&new_blocked_signals, SIGINT);
  sigaddset(&new_blocked_signals, SIGTERM);
  sigset_t prev_blocked_signals;
  pthread_sigmask(SIG_BLOCK, &new_blocked_signals, &prev_blocked_signals);
  sighandler_t prev_sigint_handler = signal(SIGINT, SatHandleBreak);
  sighandler_t prev_sigterm_handler = signal(SIGTERM, SatHandleBreak);

  // Kick off all the worker threads.
  run_step.AddLog(Log{.severity = LogSeverity::kDebug,
                      .message = "Launching worker threads"});
  InitializeThreads(run_step);
  SpawnThreads(run_step);
  pthread_sigmask(SIG_SETMASK, &prev_blocked_signals, NULL);

  run_step.AddLog(
      Log{.severity = LogSeverity::kDebug,
          .message = absl::StrFormat("Starting countdown with %d seconds",
                                     runtime_seconds_)});

  // In seconds.
  static const time_t kSleepFrequency = 5;
  // All of these are in seconds.  You probably want them to be >=
  // kSleepFrequency and multiples of kSleepFrequency, but neither is
  // necessary.
  static const time_t kInjectionFrequency = 10;
  // print_delay_ determines "seconds remaining" chatty update.

  const time_t start = time(NULL);
  const time_t end = start + runtime_seconds_;
  time_t now = start;
  time_t next_print = start + print_delay_;
  time_t next_pause = start + pause_delay_;
  time_t next_resume = 0;
  time_t next_injection;
  if (crazy_error_injection_) {
    next_injection = start + kInjectionFrequency;
  } else {
    next_injection = 0;
  }

  while (now < end) {
    // This is an int because it's for logprintf().
    const int seconds_remaining = end - now;

    if (user_break_) {
      // Handle early exit.
      run_step.AddLog(
          Log{.severity = LogSeverity::kDebug,
              .message = absl::StrFormat(
                  "User exiting early with %d seconds remaining in test",
                  seconds_remaining)});
      break;
    }

    // If we have an error limit, check it here and see if we should exit.
    if (max_errorcount_ != 0) {
      uint64 errors = GetTotalErrorCount();
      if (errors > max_errorcount_) {
        run_step.AddLog(Log{.severity = LogSeverity::kError,
                            .message = absl::StrFormat(
                                "Exiting early with %d seconds remaining in "
                                "test due to excession (%lld) errors",
                                seconds_remaining, errors)});
        break;
      }
    }

    if (now >= next_print) {
      // Print a count down message.
      run_step.AddLog(
          Log{.severity = LogSeverity::kInfo,
              .message = absl::StrFormat("%d seconds remaining in test",
                                         seconds_remaining)});
      next_print = NextOccurance(print_delay_, start, now);
    }

    if (next_injection && now >= next_injection) {
      // Inject an error.
      run_step.AddLog(
          Log{.severity = LogSeverity::kDebug,
              .message = absl::StrFormat(
                  "Injecting error with %d seconds remaining in test",
                  seconds_remaining)});
      struct page_entry src;
      GetValid(&src, run_step);
      src.pattern = patternlist_->GetPattern(0, run_step);
      PutValid(&src, run_step);
      next_injection = NextOccurance(kInjectionFrequency, start, now);
    }

    if (next_pause && now >= next_pause) {
      // Tell worker threads to pause in preparation for a power spike.
      run_step.AddLog(Log{.severity = LogSeverity::kInfo,
                          .message = absl::StrFormat(
                              "Pausing worker threads in preparation for power "
                              "spike with %d seconds remaining in test",
                              seconds_remaining)});
      power_spike_status_.PauseWorkers();
      run_step.AddLog(Log{.severity = LogSeverity::kDebug,
                          .message = "Worker threads paused"});
      next_pause = 0;
      next_resume = now + pause_duration_;
    }

    if (next_resume && now >= next_resume) {
      // Tell worker threads to resume in order to cause a power spike.
      run_step.AddLog(Log{
          .severity = LogSeverity::kInfo,
          .message = absl::StrFormat("Resuming worker threads to cause a power "
                                     "spike with %d seconds remaining in test",
                                     seconds_remaining)});
      power_spike_status_.ResumeWorkers();
      run_step.AddLog(Log{.severity = LogSeverity::kDebug,
                          .message = "Worker threads resumed"});
      next_pause = NextOccurance(pause_delay_, start, now);
      next_resume = 0;
    }

    sat_sleep(NextOccurance(kSleepFrequency, start, now) - now);
    now = time(NULL);
  }

  JoinThreads(run_step);

  if (!monitor_mode_) RunAnalysis();

  DeleteThreads(run_step);

  run_step.AddLog(Log{.severity = LogSeverity::kDebug,
                      .message = "Uninstalling signal handlers"});
  signal(SIGINT, prev_sigint_handler);
  signal(SIGTERM, prev_sigterm_handler);

  return true;
}

// Clean up all resources.
bool Sat::Cleanup() {
  g_sat = NULL;
  Logger::GlobalLogger()->StopThread();
  Logger::GlobalLogger()->SetStdoutOnly();
  if (logfile_) {
    close(logfile_);
    logfile_ = 0;
  }
  if (patternlist_) {
    patternlist_->Destroy();
    delete patternlist_;
    patternlist_ = 0;
  }
  if (os_) {
    os_->FreeTestMem();
    delete os_;
    os_ = 0;
  }
  if (empty_) {
    delete empty_;
    empty_ = 0;
  }
  if (valid_) {
    delete valid_;
    valid_ = 0;
  }
  if (finelock_q_) {
    delete finelock_q_;
    finelock_q_ = 0;
  }
  if (page_bitmap_) {
    delete[] page_bitmap_;
  }

  for (size_t i = 0; i < blocktables_.size(); i++) {
    delete blocktables_[i];
  }

  if (cc_cacheline_data_) {
    // The num integer arrays for all the cacheline structures are
    // allocated as a single chunk. The pointers in the cacheline struct
    // are populated accordingly. Hence calling free on the first
    // cacheline's num's address is going to free the entire array.
    // TODO(aganti): Refactor this to have a class for the cacheline
    // structure (currently defined in worker.h) and clean this up
    // in the destructor of that class.
    if (cc_cacheline_data_[0].num) {
      free(cc_cacheline_data_[0].num);
    }
    free(cc_cacheline_data_);
  }

  sat_assert(0 == pthread_mutex_destroy(&worker_lock_));

  return true;
}

// Helper functions.
void Sat::AcquireWorkerLock() {
  sat_assert(0 == pthread_mutex_lock(&worker_lock_));
}
void Sat::ReleaseWorkerLock() {
  sat_assert(0 == pthread_mutex_unlock(&worker_lock_));
}

void logprintf(int priority, const char *format, ...) {
  va_list args;
  va_start(args, format);
  Logger::GlobalLogger()->VLogF(priority, format, args);
  va_end(args);
}

// Stop the logging thread and verify any pending data is written to the log.
void logstop() { Logger::GlobalLogger()->StopThread(); }
