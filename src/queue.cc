// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// queue.cc : simple thread safe queue implementation

#include <stdlib.h>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "absl/strings/str_format.h"
#include "ocpdiag/core/results/data_model/input_model.h"
#include "ocpdiag/core/results/test_step.h"
#include "queue.h"
#include "sattypes.h"

using ::ocpdiag::results::Error;
using ::ocpdiag::results::TestStep;

// Page entry queue implementation follows.
// Push inserts pages, pop returns a random entry.

PageEntryQueue::PageEntryQueue(uint64 queuesize) {
  // There must always be one empty queue location,
  // since in == out => empty.
  q_size_ = queuesize + 1;
  pages_ = new struct page_entry[q_size_];
  nextin_ = 0;
  nextout_ = 0;
  popped_ = 0;
  pushed_ = 0;
  pthread_mutex_init(&q_mutex_, NULL);
}
PageEntryQueue::~PageEntryQueue() {
  delete[] pages_;
  pthread_mutex_destroy(&q_mutex_);
}

// Add a page into this queue.
int PageEntryQueue::Push(struct page_entry *pe) {
  int result = 0;
  int64 nextnextin;

  if (!pe) return 0;

  pthread_mutex_lock(&q_mutex_);
  nextnextin = (nextin_ + 1) % q_size_;

  if (nextnextin != nextout_) {
    pages_[nextin_] = *pe;

    nextin_ = nextnextin;
    result = 1;

    pushed_++;
  }

  pthread_mutex_unlock(&q_mutex_);

  return result;
}

// Retrieve a random page from this queue.
int PageEntryQueue::PopRandom(struct page_entry *pe, TestStep &test_step) {
  int result = 0;
  int64 lastin;
  int64 entries;
  int64 newindex;
  struct page_entry tmp;

  if (!pe) return 0;

  // TODO(nsanders): we should improve random to get 64 bit randoms, and make
  // it more thread friendly.
  uint64 rand = random();

  int retval = pthread_mutex_lock(&q_mutex_);
  if (retval) {
    test_step.AddError(Error{
        .symptom = kProcessError,
        .message = absl::StrFormat("pthreads mutex failure %d", retval),
    });
  }

  if (nextin_ != nextout_) {
    // Randomized fetch.
    // Swap random entry with next out.
    {
      lastin = (nextin_ - 1 + q_size_) % q_size_;
      entries = (lastin - nextout_ + q_size_) % q_size_;

      newindex = nextout_;
      if (entries) newindex = ((rand % entries) + nextout_) % q_size_;

      // Swap the pages.
      tmp = pages_[nextout_];
      pages_[nextout_] = pages_[newindex];
      pages_[newindex] = tmp;
    }

    // Return next out page.
    *pe = pages_[nextout_];

    nextout_ = (nextout_ + 1) % q_size_;
    result = 1;

    popped_++;
  }

  pthread_mutex_unlock(&q_mutex_);

  return result;
}
