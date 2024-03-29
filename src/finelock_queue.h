// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// This page entry queue implementation with fine grain locks aim to ease
// lock contention over previous queue implementation (with one lock protecting
// the entire queue).

#ifndef STRESSAPPTEST_FINELOCK_QUEUE_H_
#define STRESSAPPTEST_FINELOCK_QUEUE_H_

#include <string>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "ocpdiag/core/results/test_step.h"
#include "os.h"
#include "pattern.h"
#include "queue.h"  // Using page_entry struct.
#include "sattypes.h"

// This is a threadsafe randomized queue of pages with per-page entry lock
// for worker threads to use.
class FineLockPEQueue {
 public:
  FineLockPEQueue(uint64 queuesize, int64 pagesize);
  ~FineLockPEQueue();

  // Put and get functions for page entries.
  bool GetEmpty(struct page_entry *pe, ocpdiag::results::TestStep &test_step);
  bool GetValid(struct page_entry *pe, ocpdiag::results::TestStep &test_step);
  bool PutEmpty(struct page_entry *pe);
  bool PutValid(struct page_entry *pe);

  // Put and get functions for page entries, selecting on tags.
  bool GetEmpty(struct page_entry *pe, int32 tag,
                ocpdiag::results::TestStep &test_step);
  bool GetValid(struct page_entry *pe, int32 tag,
                ocpdiag::results::TestStep &test_step);

  bool QueueAnalysis(ocpdiag::results::TestStep &test_step);
  bool GetPageFromPhysical(uint64 paddr, struct page_entry *pe);

 private:
  // Not that much blocking random number generator.
  uint64 GetRandom64(ocpdiag::results::TestStep &test_step);
  uint64 GetRandom64FromSlot(int slot);

  // Helper function to check index range, returns true if index is valid.
  bool valid_index(int64 index) {
    return index >= 0 && static_cast<uint64>(index) < q_size_;
  }

  // Returns true if page entry is valid, false otherwise.
  static bool page_is_valid(struct page_entry *pe) {
    return pe->pattern != NULL;
  }
  // Returns true if page entry is empty, false otherwise.
  static bool page_is_empty(struct page_entry *pe) {
    return pe->pattern == NULL;
  }

  // Helper function to get a random page entry with given predicate,
  // ie, page_is_valid() or page_is_empty() as defined above.
  bool GetRandomWithPredicate(struct page_entry *pe,
                              bool (*pred_func)(struct page_entry *),
                              ocpdiag::results::TestStep &test_step);

  // Helper function to get a random page entry with given predicate,
  // ie, page_is_valid() or page_is_empty() as defined above.
  bool GetRandomWithPredicateTag(struct page_entry *pe,
                                 bool (*pred_func)(struct page_entry *),
                                 int32 tag,
                                 ocpdiag::results::TestStep &test_step);

  // Used to make a linear congruential path through the queue.
  int64 getA(int64 m);
  int64 getC(int64 m);

  pthread_mutex_t *pagelocks_;  // Per-page-entry locks.
  struct page_entry *pages_;    // Where page entries are held.
  uint64 q_size_;               // Size of the queue.
  int64 page_size_;             // For calculating array index from offset.

  enum {
    kTries = 1,  // Measure the number of attempts in the queue
                 // before getting a matching page.
    kTouch = 2
  }               // Measure the number of touches on each page.
  queue_metric_;  // What to measure in the 'tries' field.

  // Progress pseudorandomly through the queue. It's required that we can find
  // every value in the list, but progressing through the same order each time
  // causes bunching of pages, leading to long seach times for the correct
  // type of pages.
  int64 a_;          // 'a' multiplicative value for progressing
                     // linear congruentially through the list.
  int64 c_;          // 'c' additive value for prgressing randomly
                     // through the list.
  int64 modlength_;  // 'm' mod value for linear congruential
                     // generator. Used when q_size_ doesn't
                     // generate a good progression through the
                     // list.

  uint64 rand_seed_[4];           // Random number state for 4 generators.
  pthread_mutex_t randlocks_[4];  // Per-random-generator locks.

  DISALLOW_COPY_AND_ASSIGN(FineLockPEQueue);
};

#endif  // STRESSAPPTEST_FINELOCK_QUEUE_H_
