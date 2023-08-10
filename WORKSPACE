load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "ocp_diag_core_cpp",
    sha256 = "a9448d5fd226cd310caa3791ea958e7326d4d06e1cfce346a7e71766ca4691cd",
    strip_prefix = "ocp-diag-core-cpp-065fc134c9ba523f1f8e759010d80ef18bfd9655",
    urls = ["https://github.com/opencomputeproject/ocp-diag-core-cpp/archive/065fc134c9ba523f1f8e759010d80ef18bfd9655.zip"],
)

load("@ocp_diag_core_cpp//ocpdiag:build_deps.bzl", "load_deps")

load_deps("ocp_diag_core_cpp")

load("@ocp_diag_core_cpp//ocpdiag:secondary_build_deps.bzl", "load_secondary_deps")

load_secondary_deps()

load("@ocp_diag_core_cpp//ocpdiag:tertiary_build_deps.bzl", "load_tertiary_deps")

load_tertiary_deps()
