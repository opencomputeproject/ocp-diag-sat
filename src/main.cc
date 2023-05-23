// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// sat.cc : a stress test for stressful testing

#include "ocpdiag/core/results/data_model/output_model.h"
#include "sat.h"
#include "sattypes.h"

int main(int argc, const char **argv) {
  Sat *sat = SatFactory();
  if (sat == NULL) {
    std::cerr << "Process Error: failed to allocate Sat object";
    return 255;
  }

  if (!sat->ParseArgs(argc, argv)) {
    std::cerr << "Process Error: Sat::ParseArgs() failed";
    return 1;
  } else if (!sat->Initialize()) {
    return 1;
  }

  sat->Run();
  sat->Cleanup();

  int retval = 0;
  if (sat->status() == ocpdiag::results::TestResult::kFail) retval = 1;

  delete sat;
  return retval;
}
