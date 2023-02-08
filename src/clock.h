// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#ifndef STRESSAPPTEST_CLOCK_H_  // NOLINT
#define STRESSAPPTEST_CLOCK_H_

#include <time.h>

// This class implements a clock that can be overriden for unit tests.
class Clock {
 public:
  virtual ~Clock() {}

  virtual time_t Now() { return time(NULL); }
};

#endif  // STRESSAPPTEST_CLOCK_H_ NOLINT
