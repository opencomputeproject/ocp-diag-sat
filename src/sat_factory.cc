// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// sat_factory.h : factory for SAT

#include "sat.h"  // NOLINT

Sat *SatFactory() { return new Sat(); }
