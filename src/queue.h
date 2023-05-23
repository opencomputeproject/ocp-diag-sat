// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// queue.h : simple queue api

// This is an interface to a simple thread safe queue,
// used to hold data blocks and patterns.
// The order in which the blocks are returned is random.

#ifndef STRESSAPPTEST_QUEUE_H_  // NOLINT
#define STRESSAPPTEST_QUEUE_H_

#include <pthread.h>
#include <sys/types.h>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "ocpdiag/core/results/test_step.h"
#include "pattern.h"   // NOLINT
#include "sattypes.h"  // NOLINT

// Tag indicating no preference.
static const int kDontCareTag = -1;
// Tag indicating no preference.
static const int kInvalidTag = 0xf001;

// This describes a block of memory, and the expected fill pattern.
struct page_entry {
  uint64 offset;
  void *addr;
  uint64 paddr;
  class Pattern *pattern;
  int32 tag;       // These are tags for use in NUMA affinity or other uses.
  uint32 touch;    // Counter of the number of reads from this page.
  uint64 ts;       // Timestamp of the last read from this page.
  uint32 lastcpu;  // Last CPU to write this page.
  class Pattern *lastpattern;  // Expected Pattern at last read.
};

static inline void init_pe(struct page_entry *pe) {
  pe->offset = 0;
  pe->addr = NULL;
  pe->pattern = NULL;
  pe->tag = kInvalidTag;
  pe->touch = 0;
  pe->ts = 0;
  pe->lastcpu = 0;
}

// This is a threadsafe randomized queue of pages for
// worker threads to use.
class PageEntryQueue {
 public:
  explicit PageEntryQueue(uint64 queuesize);
  ~PageEntryQueue();

  // Push a page onto the list.
  int Push(struct page_entry *pe);
  // Pop a random page off of the list.
  int PopRandom(struct page_entry *pe, ocpdiag::results::TestStep &test_step);

 private:
  struct page_entry *pages_;  // Where the pages are held.
  int64 nextin_;
  int64 nextout_;
  int64 q_size_;  // Size of the queue.
  int64 pushed_;  // Number of pages pushed, total.
  int64 popped_;  // Number of pages popped, total.
  pthread_mutex_t q_mutex_;

  DISALLOW_COPY_AND_ASSIGN(PageEntryQueue);
};

#endif  // MILES_TESTS_SAT_QUEUE_H_ NOLINT
