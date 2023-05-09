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
    deps = [
        "@com_google_absl//absl/strings:str_format",
        "@ocp_diag_core//ocpdiag/core/results:measurement_series",
        "@ocp_diag_core//ocpdiag/core/results:test_run",
        "@ocp_diag_core//ocpdiag/core/results:test_step",
        "@ocp_diag_core//ocpdiag/core/results/data_model:dut_info",
        "@ocp_diag_core//ocpdiag/core/results/data_model:input_model",
        "@ocp_diag_core//ocpdiag/core/results/data_model:input_model_helpers",
        "@ocp_diag_core//ocpdiag/core/results/data_model:output_model",
    ],
)

cc_binary(
    name = "sat_bin",
    srcs = ["main.cc"],
    deps = [":sat_lib"],
)
