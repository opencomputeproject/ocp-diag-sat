// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// worker.h : worker thread interface

// This file contains the Worker Thread class interface
// for the SAT test. Worker Threads implement a repetative
// task used to test or stress the system.

#ifndef STRESSAPPTEST_WORKER_H_
#define STRESSAPPTEST_WORKER_H_

#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef HAVE_LIBAIO_H
#include <libaio.h>
#endif

#include <queue>
#include <set>
#include <string>
#include <vector>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "disk_blocks.h"
#include "ocpdiag/core/results/data_model/input_model.h"
#include "ocpdiag/core/results/measurement_series.h"
#include "ocpdiag/core/results/test_step.h"
#include "queue.h"
#include "sattypes.h"

// Global Datastruture shared by the Cache Coherency Worker Threads.
struct cc_cacheline_data {
  char *num;
};

// Typical usage:
// (Other workflows may be possible, see function comments for details.)
// - Control thread creates object.
// - Control thread calls AddWorkers(1) for each worker thread.
// - Control thread calls Initialize().
// - Control thread launches worker threads.
// - Every worker thread frequently calls ContinueRunning().
// - Control thread periodically calls PauseWorkers(), effectively sleeps, and
//     then calls ResumeWorkers().
// - Some worker threads may exit early, before StopWorkers() is called.  They
//     call RemoveSelf() after their last call to ContinueRunning().
// - Control thread eventually calls StopWorkers().
// - Worker threads exit.
// - Control thread joins worker threads.
// - Control thread calls Destroy().
// - Control thread destroys object.
//
// Threadsafety:
// - ContinueRunning() may be called concurrently by different workers, but not
//     by a single worker.
// - No other methods may ever be called concurrently, with themselves or
//     eachother.
// - This object may be used by multiple threads only between Initialize() and
//     Destroy().
//
// TODO(matthewb): Move this class and its unittest to their own files.
class WorkerStatus {
 public:
  //--------------------------------
  // Methods for the control thread.
  //--------------------------------

  WorkerStatus() : num_workers_(0), status_(RUN) {}

  // Called by the control thread to increase the worker count.  Must be called
  // before Initialize().  The worker count is 0 upon object initialization.
  void AddWorkers(int num_new_workers) {
    // No need to lock num_workers_mutex_ because this is before Initialize().
    num_workers_ += num_new_workers;
  }

  // Called by the control thread.  May not be called multiple times.  If
  // called, Destroy() must be called before destruction.
  void Initialize();

  // Called by the control thread after joining all worker threads.  Must be
  // called iff Initialize() was called.  No methods may be called after calling
  // this.
  void Destroy();

  // Called by the control thread to tell the workers to pause.  Does not return
  // until all workers have called ContinueRunning() or RemoveSelf().  May only
  // be called between Initialize() and Stop().  Must not be called multiple
  // times without ResumeWorkers() having been called inbetween.
  void PauseWorkers();

  // Called by the control thread to tell the workers to resume from a pause.
  // May only be called between Initialize() and Stop().  May only be called
  // directly after PauseWorkers().
  void ResumeWorkers();

  // Called by the control thread to tell the workers to stop.  May only be
  // called between Initialize() and Destroy().  May only be called once.
  void StopWorkers();

  //--------------------------------
  // Methods for the worker threads.
  //--------------------------------

  // Called by worker threads to decrease the worker count by one.  May only be
  // called between Initialize() and Destroy().  May wait for ResumeWorkers()
  // when called after PauseWorkers().
  void RemoveSelf();

  // Called by worker threads between Initialize() and Destroy().  May be called
  // any number of times.  Return value is whether or not the worker should
  // continue running.  When called after PauseWorkers(), does not return until
  // ResumeWorkers() or StopWorkers() has been called.  Number of distinct
  // calling threads must match the worker count (see AddWorkers() and
  // RemoveSelf()).
  bool ContinueRunning(bool *paused);

  // This is a hack!  It's like ContinueRunning(), except it won't pause.  If
  // any worker threads use this exclusively in place of ContinueRunning() then
  // PauseWorkers() should never be used!
  bool ContinueRunningNoPause();

 private:
  enum Status { RUN, PAUSE, STOP };

  void WaitOnPauseBarrier() {
#ifdef HAVE_PTHREAD_BARRIERS
    pthread_rwlock_rdlock(&pause_rwlock_);
    int error = pthread_barrier_wait(&pause_barrier_);
    if (error != PTHREAD_BARRIER_SERIAL_THREAD) sat_assert(error == 0);
    pthread_rwlock_unlock(&pause_rwlock_);
#endif
  }

  void AcquireNumWorkersLock() {
    sat_assert(0 == pthread_mutex_lock(&num_workers_mutex_));
  }

  void ReleaseNumWorkersLock() {
    sat_assert(0 == pthread_mutex_unlock(&num_workers_mutex_));
  }

  void AcquireStatusReadLock() {
    sat_assert(0 == pthread_rwlock_rdlock(&status_rwlock_));
  }

  void AcquireStatusWriteLock() {
    sat_assert(0 == pthread_rwlock_wrlock(&status_rwlock_));
  }

  void ReleaseStatusLock() {
    sat_assert(0 == pthread_rwlock_unlock(&status_rwlock_));
  }

  Status GetStatus() {
    AcquireStatusReadLock();
    Status status = status_;
    ReleaseStatusLock();
    return status;
  }

  // Returns the previous status.
  Status SetStatus(Status status) {
    AcquireStatusWriteLock();
    Status prev_status = status_;
    status_ = status;
    ReleaseStatusLock();
    return prev_status;
  }

  pthread_mutex_t num_workers_mutex_;
  int num_workers_;

  pthread_rwlock_t status_rwlock_;
  Status status_;

#ifdef HAVE_PTHREAD_BARRIERS
  pthread_barrier_t pause_barrier_;
  pthread_rwlock_t pause_rwlock_;  // Guards pause_barrier_
#endif

  DISALLOW_COPY_AND_ASSIGN(WorkerStatus);
};

// This is a base class for worker threads.
// Each thread repeats a specific
// task on various blocks of memory.
class WorkerThread {
 public:
  // Enum to mark a thread as low/med/high priority.
  enum Priority {
    Low,
    Normal,
    High,
  };
  WorkerThread();
  virtual ~WorkerThread();

  // Initialize values and thread ID number.
  virtual void InitThread(int thread_num_init, class Sat *sat_init,
                          class OsLayer *os_init,
                          class PatternList *patternlist_init,
                          WorkerStatus *worker_status,
                          ocpdiag::results::TestStep *test_step);

  // This function is DEPRECATED, it does nothing.
  void SetPriority(Priority priority) { priority_ = priority; }
  // Spawn the worker thread, by running Work().
  int SpawnThread();
  // Only for ThreadSpawnerGeneric().
  void StartRoutine();
  bool InitPriority();

  // Wait for the thread to complete its cleanup.
  virtual bool JoinThread();
  // Kill worker thread with SIGINT.
  virtual bool KillThread();

  // This is the task function that the thread executes.
  // This is implemented per subclass.
  virtual bool Work();

  // Starts per-WorkerThread timer.
  void StartThreadTimer() { start_time_ = sat_get_time_us(); }
  // Reads current timer value and returns run duration without recording it.
  int64 ReadThreadTimer() {
    int64 end_time_ = sat_get_time_us();
    return end_time_ - start_time_;
  }
  // Stops per-WorkerThread timer and records thread run duration.
  // Start/Stop ThreadTimer repetitively has cumulative effect, ie the timer
  // is effectively paused and restarted, so runduration_usec accumulates on.
  void StopThreadTimer() { runduration_usec_ += ReadThreadTimer(); }

  // Acccess member variables.
  bool GetStatus() { return status_; }
  int64 GetErrorCount() { return errorcount_; }
  int64 GetPageCount() { return pages_copied_; }
  int64 GetRunDurationUSec() { return runduration_usec_; }
  virtual string GetThreadTypeName() { return "Generic Worker Thread"; }

  // Returns bandwidth defined as pages_copied / thread_run_durations.
  virtual float GetCopiedData();
  // Calculate worker thread specific copied data.
  virtual float GetMemoryCopiedData() { return 0; }
  virtual float GetDeviceCopiedData() { return 0; }
  // Calculate worker thread specific bandwidth.
  virtual float GetMemoryBandwidth() {
    return GetMemoryCopiedData() / (runduration_usec_ * 1.0 / 1000000.);
  }
  virtual float GetDeviceBandwidth() {
    return GetDeviceCopiedData() / (runduration_usec_ * 1.0 / 1000000.);
  }

  void set_cpu_mask(cpu_set_t *mask) {
    memcpy(&cpu_mask_, mask, sizeof(*mask));
  }

  void set_cpu_mask_to_cpu(int cpu_num) {
    cpuset_set_ab(&cpu_mask_, cpu_num, cpu_num + 1);
  }

  void set_tag(int32 tag) { tag_ = tag; }

  // Returns CPU mask, where each bit represents a logical cpu.
  bool AvailableCpus(cpu_set_t *cpuset);
  // Returns CPU mask of CPUs this thread is bound to,
  bool CurrentCpus(cpu_set_t *cpuset);
  // Returns Current Cpus mask as string.
  string CurrentCpusFormat() {
    cpu_set_t current_cpus;
    CurrentCpus(&current_cpus);
    return cpuset_format(&current_cpus);
  }

  int ThreadID() { return thread_num_; }

  // Bind worker thread to specified CPU(s)
  bool BindToCpus(const cpu_set_t *cpuset);

 protected:
  // This function dictates whether the main work loop
  // continues, waits, or terminates.
  // All work loops should be of the form:
  //   do {
  //     // work.
  //   } while (IsReadyToRun());
  virtual bool IsReadyToRun(bool *paused = NULL) {
    return worker_status_->ContinueRunning(paused);
  }

  // Like IsReadyToRun(), except it won't pause.
  virtual bool IsReadyToRunNoPause() {
    return worker_status_->ContinueRunningNoPause();
  }

  // These are functions used by the various work loops.
  // Pretty print and log a data miscompare.
  virtual void ProcessError(struct ErrorRecord *er, const char *message);

  // Compare a region of memory with a known data patter, and report errors.
  virtual int CheckRegion(void *addr, class Pattern *pat, uint32 lastcpu,
                          int64 length, int offset, int64 patternoffset);

  // Fast compare a block of memory.
  virtual int CrcCheckPage(struct page_entry *srcpe);

  // Fast copy a block of memory, while verifying correctness.
  virtual int CrcCopyPage(struct page_entry *dstpe, struct page_entry *srcpe);

  // Fast copy a block of memory, while verifying correctness, and heating CPU.
  virtual int CrcWarmCopyPage(struct page_entry *dstpe,
                              struct page_entry *srcpe);

  // Fill a page with its specified pattern.
  virtual bool FillPage(struct page_entry *pe);

  // Copy with address tagging.
  virtual bool AdlerAddrMemcpyC(uint64 *dstmem64, uint64 *srcmem64,
                                unsigned int size_in_bytes,
                                AdlerChecksum *checksum, struct page_entry *pe);
  // SSE copy with address tagging.
  virtual bool AdlerAddrMemcpyWarm(uint64 *dstmem64, uint64 *srcmem64,
                                   unsigned int size_in_bytes,
                                   AdlerChecksum *checksum,
                                   struct page_entry *pe);
  // Crc data with address tagging.
  virtual bool AdlerAddrCrcC(uint64 *srcmem64, unsigned int size_in_bytes,
                             AdlerChecksum *checksum, struct page_entry *pe);
  // Setup tagging on an existing page.
  virtual bool TagAddrC(uint64 *memwords, unsigned int size_in_bytes);
  // Report a mistagged cacheline.
  virtual bool ReportTagError(uint64 *mem64, uint64 actual, uint64 tag);
  // Print out the error record of the tag mismatch.
  virtual void ProcessTagError(struct ErrorRecord *error, const char *message);

  // A worker thread can yield itself to give up CPU until it's scheduled again
  bool YieldSelf();

  void AddLog(ocpdiag::results::LogSeverity severity, const string &message);

  void AddProcessError(const string &message);

  void AddDiagnosis(const string &verdict, ocpdiag::results::DiagnosisType type,
                    const string &message);

 protected:
  // General state variables that all subclasses need.
  int thread_num_;               // Thread ID.
  volatile bool status_;         // Error status.
  volatile int64 pages_copied_;  // Recorded for memory bandwidth calc.
  volatile int64 errorcount_;    // Miscompares seen by this thread.

  cpu_set_t cpu_mask_;   // Cores this thread is allowed to run on.
  volatile uint32 tag_;  // Tag hint for memory this thread can use.

  bool tag_mode_;  // Tag cachelines with vaddr.

  // Thread timing variables.
  int64 start_time_;                 // Worker thread start time.
  volatile int64 runduration_usec_;  // Worker run duration in u-seconds.

  // Function passed to pthread_create.
  void *(*thread_spawner_)(void *args);
  pthread_t thread_;                // Pthread thread ID.
  Priority priority_;               // Worker thread priority.
  class Sat *sat_;                  // Reference to parent stest object.
  class OsLayer *os_;               // Os abstraction: put hacks here.
  class PatternList *patternlist_;  // Reference to data patterns.

  ocpdiag::results::TestStep *test_step_;  // The OCP diag test step.

  // Work around style guide ban on sizeof(int).
  static const uint64 iamint_ = 0;
  static const int wordsize_ = sizeof(iamint_);

 private:
  WorkerStatus *worker_status_;

  DISALLOW_COPY_AND_ASSIGN(WorkerThread);
};

// Worker thread to perform File IO.
class FileThread : public WorkerThread {
 public:
  FileThread();
  // Set filename to use for file IO.
  virtual void SetFile(const string &filename_init);
  virtual bool Work();

  // Calculate worker thread specific bandwidth.
  virtual float GetDeviceCopiedData() { return GetCopiedData() * 2; }
  virtual float GetMemoryCopiedData();

  string GetThreadTypeName() { return "File IO Thread"; }

 protected:
  // Record of where these pages were sourced from, and what
  // potentially broken components they passed through.
  struct PageRec {
    class Pattern *pattern;  // This is the data it should contain.
    void *src;  // This is the memory location the data was sourced from.
    void *dst;  // This is where it ended up.
  };

  // These are functions used by the various work loops.
  // Pretty print and log a data miscompare. Disks require
  // slightly different error handling.
  virtual void ProcessError(struct ErrorRecord *er, const char *message);

  virtual bool OpenFile(int *pfile);
  virtual bool CloseFile(int fd);

  // Read and write whole file to disk.
  virtual bool WritePages(int fd);
  virtual bool ReadPages(int fd);

  // Read and write pages to disk.
  virtual bool WritePageToFile(int fd, struct page_entry *src);
  virtual bool ReadPageFromFile(int fd, struct page_entry *dst);

  // Sector tagging support.
  virtual bool SectorTagPage(struct page_entry *src, int block);
  virtual bool SectorValidatePage(const struct PageRec &page,
                                  struct page_entry *dst, int block);

  // Get memory for an incoming data transfer..
  virtual bool PagePrepare();
  // Remove memory allocated for data transfer.
  virtual bool PageTeardown();

  // Get memory for an incoming data transfer..
  virtual bool GetEmptyPage(struct page_entry *dst);
  // Get memory for an outgoing data transfer..
  virtual bool GetValidPage(struct page_entry *dst);
  // Throw out a used empty page.
  virtual bool PutEmptyPage(struct page_entry *src);
  // Throw out a used, filled page.
  virtual bool PutValidPage(struct page_entry *src);

  struct PageRec *page_recs_;  // Array of page records.
  int crc_page_;               // Page currently being CRC checked.
  string filename_;            // Name of file to access.

  bool page_io_;      // Use page pool for IO.
  void *local_page_;  // malloc'd page fon non-pool IO.
  int pass_;          // Number of writes to the file so far.

  // Tag to detect file corruption.
  struct SectorTag {
    volatile uint8 magic;
    volatile uint8 block;
    volatile uint8 sector;
    volatile uint8 pass;
    char pad[512 - 4];
  };

  DISALLOW_COPY_AND_ASSIGN(FileThread);
};

// Worker thread to perform Network IO.
class NetworkThread : public WorkerThread {
 public:
  NetworkThread();
  // Set hostname to use for net IO.
  virtual void SetIP(const char *ipaddr_init);
  virtual bool Work();

  // Calculate worker thread specific bandwidth.
  virtual float GetDeviceCopiedData() { return GetCopiedData() * 2; }

  string GetThreadTypeName() { return "Network IO Thread"; }

 protected:
  // IsReadyToRunNoPause() wrapper, for NetworkSlaveThread to override.
  virtual bool IsNetworkStopSet();
  virtual bool CreateSocket(int *psocket);
  virtual bool CloseSocket(int sock);
  virtual bool Connect(int sock);
  virtual bool SendPage(int sock, struct page_entry *src);
  virtual bool ReceivePage(int sock, struct page_entry *dst);
  char ipaddr_[256];
  int sock_;

 private:
  DISALLOW_COPY_AND_ASSIGN(NetworkThread);
};

// Worker thread to reflect Network IO.
class NetworkSlaveThread : public NetworkThread {
 public:
  NetworkSlaveThread();
  // Set socket for IO.
  virtual void SetSock(int sock);
  virtual bool Work();

  string GetThreadTypeName() { return "Child Network Thread"; }

 protected:
  virtual bool IsNetworkStopSet();

 private:
  DISALLOW_COPY_AND_ASSIGN(NetworkSlaveThread);
};

// Worker thread to detect incoming Network IO.
class NetworkListenThread : public NetworkThread {
 public:
  NetworkListenThread();
  virtual bool Work();

  string GetThreadTypeName() { return "Network Listen Thread"; }

 private:
  virtual bool Listen();
  virtual bool Wait();
  virtual bool GetConnection(int *pnewsock);
  virtual bool SpawnSlave(int newsock, int threadid);
  virtual bool ReapSlaves();

  // For serviced incoming connections.
  struct ChildWorker {
    WorkerStatus status;
    NetworkSlaveThread thread;
  };
  typedef vector<ChildWorker *> ChildVector;
  ChildVector child_workers_;

  DISALLOW_COPY_AND_ASSIGN(NetworkListenThread);
};

// Worker thread to perform Memory Copy.
class CopyThread : public WorkerThread {
 public:
  CopyThread() {}
  virtual bool Work();
  // Calculate worker thread specific bandwidth.
  virtual float GetMemoryCopiedData() { return GetCopiedData() * 2; }

 protected:
  string GetThreadTypeName() { return "Memory Copy Thread"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(CopyThread);
};

// Worker thread to perform Memory Invert.
class InvertThread : public WorkerThread {
 public:
  InvertThread() {}
  virtual bool Work();
  // Calculate worker thread specific bandwidth.
  virtual float GetMemoryCopiedData() { return GetCopiedData() * 4; }

  string GetThreadTypeName() { return "Memory Page Invert Thread"; }

 private:
  virtual int InvertPageUp(struct page_entry *srcpe);
  virtual int InvertPageDown(struct page_entry *srcpe);
  DISALLOW_COPY_AND_ASSIGN(InvertThread);
};

// Worker thread to fill blank pages on startup.
class FillThread : public WorkerThread {
 public:
  FillThread();
  // Set how many pages this thread should fill before exiting.
  virtual void SetFillPages(int64 num_pages_to_fill_init);
  virtual bool Work();

 protected:
  string GetThreadTypeName() { return "Memory Page Fill Thread"; }

 private:
  // Fill a page with the data pattern in pe->pattern.
  virtual bool FillPageRandom(struct page_entry *pe);
  int64 num_pages_to_fill_;
  DISALLOW_COPY_AND_ASSIGN(FillThread);
};

// Worker thread to verify page data matches pattern data.
// Thread will check and replace pages until "done" flag is set,
// then it will check and discard pages until no more remain.
class CheckThread : public WorkerThread {
 public:
  CheckThread() {}
  virtual bool Work();
  // Calculate worker thread specific bandwidth.
  virtual float GetMemoryCopiedData() { return GetCopiedData(); }

  string GetThreadTypeName() { return "Memory Page Check Thread"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckThread);
};

// Computation intensive worker thread to stress CPU.
class CpuStressThread : public WorkerThread {
 public:
  CpuStressThread() {}
  virtual bool Work();

  string GetThreadTypeName() { return "CPU Stress Thread"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(CpuStressThread);
};

// Worker thread that tests the correctness of the
// CPU Cache Coherency Protocol.
class CpuCacheCoherencyThread : public WorkerThread {
 public:
  CpuCacheCoherencyThread(cc_cacheline_data *cc_data, int cc_cacheline_count_,
                          int cc_thread_num_, int cc_thread_count_,
                          int cc_inc_count_);
  virtual bool Work();

  string GetThreadTypeName() { return "CPU Cache Coherency Thread"; }

 protected:
  // Used by the simple random number generator as a shift feedback;
  // this polynomial (x^64 + x^63 + x^61 + x^60 + 1) will produce a
  // psuedorandom cycle of period 2^64-1.
  static const uint64 kRandomPolynomial = 0xD800000000000000ULL;
  // A very simple psuedorandom generator that can be inlined and use
  // registers, to keep the CC test loop tight and focused.
  static uint64 SimpleRandom(uint64 seed);

  cc_cacheline_data *cc_cacheline_data_;  // Datstructure for each cacheline.
  int cc_local_num_;                      // Local counter for each thread.
  int cc_cacheline_count_;  // Number of cache lines to operate on.
  int cc_thread_num_;       // The integer id of the thread which is
                            // used as an index into the integer array
                            // of the cacheline datastructure.
  int cc_thread_count_;     // Total number of threads being run, for
                            // calculations mixing up cache line access.
  int cc_inc_count_;        // Number of times to increment the counter.

 private:
  DISALLOW_COPY_AND_ASSIGN(CpuCacheCoherencyThread);
};

// Worker thread to perform disk test.
class DiskThread : public WorkerThread {
 public:
  explicit DiskThread(DiskBlockTable *block_table);
  virtual ~DiskThread();
  // Calculate disk thread specific bandwidth.
  virtual float GetDeviceCopiedData() {
    return (blocks_written_ * write_block_size_ +
            blocks_read_ * read_block_size_) /
           kMegabyte;
  }

  // Set filename for device file (in /dev).
  virtual void SetDevice(const char *device_name);
  // Set various parameters that control the behaviour of the test.
  virtual bool SetParameters(int read_block_size, int write_block_size,
                             int64 segment_size, int64 cache_size,
                             int blocks_per_segment, int64 read_threshold,
                             int64 write_threshold, int non_destructive);

  virtual bool Work();

  virtual float GetMemoryCopiedData() { return 0; }

 protected:
  static const int kSectorSize = 512;       // Size of sector on disk.
  static const int kBufferAlignment = 512;  // Buffer alignment required by the
                                            // kernel.
  static const int kBlockRetry = 100;       // Number of retries to allocate
                                            // sectors.

  enum IoOp { ASYNC_IO_READ = 0, ASYNC_IO_WRITE = 1 };

  string GetThreadTypeName() { return "Disk Test Thread"; }

  virtual bool OpenDevice(int *pfile);
  virtual bool CloseDevice(int fd);

  // Retrieves the size (in bytes) of the disk/file.
  virtual bool GetDiskSize(int fd);

  // Retrieves the current time in microseconds.
  virtual int64 GetTime();

  // Do an asynchronous disk I/O operation.
  virtual bool AsyncDiskIO(IoOp op, int fd, void *buf, int64 size, int64 offset,
                           int64 timeout);

  // Write a block to disk.
  virtual bool WriteBlockToDisk(int fd, BlockData *block);

  // Verify a block on disk.
  virtual bool ValidateBlockOnDisk(int fd, BlockData *block);

  // Main work loop.
  virtual bool DoWork(int fd);

  int read_block_size_;     // Size of blocks read from disk, in bytes.
  int write_block_size_;    // Size of blocks written to disk, in bytes.
  int64 blocks_read_;       // Number of blocks read in work loop.
  int64 blocks_written_;    // Number of blocks written in work loop.
  int64 segment_size_;      // Size of disk segments (in bytes) that the disk
                            // will be split into where testing can be
                            // confined to a particular segment.
                            // Allows for control of how evenly the disk will
                            // be tested.  Smaller segments imply more even
                            // testing (less random).
  int blocks_per_segment_;  // Number of blocks that will be tested per
                            // segment.
  int cache_size_;          // Size of disk cache, in bytes.
  int queue_size_;          // Length of in-flight-blocks queue, in blocks.
  int non_destructive_;     // Use non-destructive mode or not.
  int update_block_table_;  // If true, assume this is the thread
                            // responsible for writing the data in the disk
                            // for this block device and, therefore,
                            // update the block table. If false, just use
                            // the block table to get data.

  // read/write times threshold for reporting a problem
  int64 read_threshold_;   // Maximum time a read should take (in us) before
                           // a warning is given.
  int64 write_threshold_;  // Maximum time a write should take (in us) before
                           // a warning is given.
  int64 read_timeout_;     // Maximum time a read can take before a timeout
                           // and the aborting of the read operation.
  int64 write_timeout_;    // Maximum time a write can take before a timeout
                           // and the aborting of the write operation.

  string device_name_;    // Name of device file to access.
  int64 device_sectors_;  // Number of sectors on the device.

  std::unique_ptr<ocpdiag::results::MeasurementSeries>
      read_times_;  // Measurement series for storing disk read times
  std::unique_ptr<ocpdiag::results::MeasurementSeries>
      write_times_;  // Measurement series for storing disk write times

  std::queue<BlockData *> in_flight_sectors_;  // Queue of sectors written but
                                               // not verified.
  void *block_buffer_;  // Pointer to aligned block buffer.

#ifdef HAVE_LIBAIO_H
  io_context_t aio_ctx_;  // Asynchronous I/O context for Linux native AIO.
#endif

  DiskBlockTable *block_table_;  // Disk Block Table, shared by all disk
                                 // threads that read / write at the same
                                 // device

  DISALLOW_COPY_AND_ASSIGN(DiskThread);
};

class RandomDiskThread : public DiskThread {
 public:
  explicit RandomDiskThread(DiskBlockTable *block_table);
  virtual ~RandomDiskThread();
  // Main work loop.
  virtual bool DoWork(int fd);

 protected:
  string GetThreadTypeName() { return "Random Disk Test Thread"; }

  DISALLOW_COPY_AND_ASSIGN(RandomDiskThread);
};

// Worker thread to check that the frequency of every cpu does not go below a
// certain threshold.
class CpuFreqThread : public WorkerThread {
 public:
  CpuFreqThread(int num_cpus, int freq_threshold, int round);
  ~CpuFreqThread();

  // This is the task function that the thread executes.
  virtual bool Work();

  // Returns true if this test can run on the current machine. Otherwise,
  // returns false.
  static bool CanRun(ocpdiag::results::TestStep &test_step);

 private:
  static const int kIntervalPause = 10;   // The number of seconds to pause
                                          // between acquiring the MSR data.
  static const int kStartupDelay = 5;     // The number of seconds to wait
                                          // before acquiring MSR data.
  static const int kMsrTscAddr = 0x10;    // The address of the TSC MSR.
  static const int kMsrAperfAddr = 0xE8;  // The address of the APERF MSR.
  static const int kMsrMperfAddr = 0xE7;  // The address of the MPERF MSR.

  // The index values into the CpuDataType.msr[] array.
  enum MsrValues {
    kMsrTsc = 0,    // MSR index 0 = TSC.
    kMsrAperf = 1,  // MSR index 1 = APERF.
    kMsrMperf = 2,  // MSR index 2 = MPERF.
    kMsrLast,       // Last MSR index.
  };

  typedef struct {
    uint32 msr;        // The address of the MSR.
    const char *name;  // A human readable string for the MSR.
  } CpuRegisterType;

  typedef struct {
    uint64 msrs[kMsrLast];  // The values of the MSRs.
    struct timeval tv;      // The time at which the MSRs were read.
  } CpuDataType;

  // The set of MSR addresses and register names.
  static const CpuRegisterType kCpuRegisters[kMsrLast];

  // Compute the change in values of the MSRs between current and previous,
  // set the frequency in MHz of the cpu. If there is an error computing
  // the delta, return false. Othewise, return true.
  bool ComputeFrequency(CpuDataType *current, CpuDataType *previous,
                        int *frequency);

  // Get the MSR values for this particular cpu and save them in data. If
  // any error is encountered, returns false. Otherwise, returns true.
  bool GetMsrs(int cpu, CpuDataType *data);

  // Compute the difference between the currently read MSR values and the
  // previously read values and store the results in delta. If any of the
  // values did not increase, or the TSC value is too small, returns false.
  // Otherwise, returns true.
  bool ComputeDelta(CpuDataType *current, CpuDataType *previous,
                    CpuDataType *delta);

  // The total number of cpus on the system.
  int num_cpus_;

  // The minimum frequency that each cpu must operate at (in MHz).
  int freq_threshold_;

  // The value to round the computed frequency to.
  int round_;

  // Precomputed value to add to the frequency to do the rounding.
  double round_value_;

  DISALLOW_COPY_AND_ASSIGN(CpuFreqThread);
};

#endif  // STRESSAPPTEST_WORKER_H_
