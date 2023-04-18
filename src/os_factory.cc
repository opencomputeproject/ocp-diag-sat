// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// This file generates an OS interface class consistant with the
// current machine type. No machine type detection is currently done.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <map>
#include <string>

#include "os.h"

// Select the proper OS and hardware interface.
OsLayer *OsLayerFactory(const std::map<std::string, std::string> &options) {
  OsLayer *os = 0;
  os = new OsLayer();

  // Check for memory allocation failure.
  if (!os) return 0;
  return os;
}
