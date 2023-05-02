// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// sat.cc : a stress test for stressful testing

#include "sat.h"
#include "sattypes.h"

int main(int argc, const char **argv) {
  Sat *sat = SatFactory();
  if (sat == NULL) {
    logprintf(0, "Process Error: failed to allocate Sat object\n");
    return 255;
  }

  if (!sat->ParseArgs(argc, argv)) {
    logprintf(0, "Process Error: Sat::ParseArgs() failed\n");
    sat->bad_status();
  } else if (!sat->Initialize()) {
    logprintf(0, "Process Error: Sat::Initialize() failed\n");
    sat->bad_status();
  } else if (!sat->Run()) {
    logprintf(0, "Process Error: Sat::Run() failed\n");
    sat->bad_status();
  }
  if (!sat->Cleanup()) {
    logprintf(0, "Process Error: Sat::Cleanup() failed\n");
    sat->bad_status();
  }

  int retval;
  if (sat->status() != 0) {
    logprintf(0,
              "Process Error: Fatal issue encountered. See above logs for "
              "details.\n");
    retval = 1;
  } else if (sat->errors() != 0) {
    retval = 1;
  } else {
    retval = 0;
  }

  delete sat;
  return retval;
}
