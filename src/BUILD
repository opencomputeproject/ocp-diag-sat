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

        "adler32memcpy.h",
        "clock.h",
        "disk_blocks.h",
        "finelock_queue.h",
        "logger.h",
        "os.h",
        "pattern.h",
        "queue.h",
        "worker.h",
    ],
    hdrs = [
        "sat.h",
        "sattypes.h",
    ],
    deps = [
        "@com_google_absl//absl/strings:str_format",
        "@ocp_diag_core_cpp//ocpdiag/core/results:measurement_series",
        "@ocp_diag_core_cpp//ocpdiag/core/results:test_run",
        "@ocp_diag_core_cpp//ocpdiag/core/results:test_step",
        "@ocp_diag_core_cpp//ocpdiag/core/results/data_model:dut_info",
        "@ocp_diag_core_cpp//ocpdiag/core/results/data_model:input_model",
        "@ocp_diag_core_cpp//ocpdiag/core/results/data_model:input_model_helpers",
        "@ocp_diag_core_cpp//ocpdiag/core/results/data_model:output_model",
        ":stressapptest_config.h__lib",
    ],
    visibility = [],
)

cc_binary(
    name = "ocpdiag_sat_x86_64",
    srcs = ["main.cc"],
    deps = [":sat_lib"],
    visibility = [],
)

genrule(
    name = "stressapptest_config.h__gen",
    srcs = [
        "autoconf/configure.ac",
        "autoconf/Makefile.am",
    ],
    outs = ["stressapptest_config.h"],
    cmd = """
        cp src/autoconf/* .
        autoreconf --install
        ./configure
        cp stressapptest_config.h $@
    """,
    visibility = [],
)

cc_library(
    name = "stressapptest_config.h__lib",
    hdrs = ["stressapptest_config.h"],
    include_prefix = ".",
    visibility = [],
)
