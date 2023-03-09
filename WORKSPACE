load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "ocp_diag_core",
    sha256 = "294b9b47e6d8cd49cefe48bd67f48e11fd1139b362f84c64c46a2566c5d26e14",
    strip_prefix = "ocp-diag-core-3c3ae8d0464fd45536179a1309ca957a4fcbd676/apis/c++",
    urls = ["https://github.com/opencomputeproject/ocp-diag-core/archive/3c3ae8d0464fd45536179a1309ca957a4fcbd676.zip"],  # 2023-03-08
)

load("@ocp_diag_core//ocpdiag:build_deps.bzl", "load_deps")

load_deps("ocp_diag_core")

load("@ocp_diag_core//ocpdiag:secondary_build_deps.bzl", "load_secondary_deps")

load_secondary_deps()

load("@ocp_diag_core//ocpdiag:tertiary_build_deps.bzl", "load_tertiary_deps")

load_tertiary_deps()
