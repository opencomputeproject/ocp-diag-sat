# Copyright 2023 Google LLC
#
# Use of this source code is governed by an MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT.

cc_library(
    name = "sat_lib",
    srcs = [
        "adler32memcpy.cc",
        "disk_blocks.cc",
        "error_diag.cc",
        "finelock_queue.cc",
        "logger.cc",
        "os.cc",
        "os_factory.cc",
        "pattern.cc",
        "queue.cc",
        "sat.cc",
        "sat_factory.cc",
        "worker.cc",
    ],
    hdrs = [
        "adler32memcpy.h",
        "clock.h",
        "disk_blocks.h",
        "error_diag.h",
        "finelock_queue.h",
        "logger.h",
        "os.h",
        "pattern.h",
        "queue.h",
        "sat.h",
        "sattypes.h",
        "stressapptest_config.h",
        "worker.h",
    ],
)

cc_binary(
    name = "sat_bin",
    srcs = ["main.cc"],
    deps = [":sat_lib"],
)
