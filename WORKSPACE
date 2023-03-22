load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "ocp_diag_core",
    sha256 = "af3eb900688c2d11c97ef51a93003cd4725f4756ee7d5c4d5fe389f10cb11995",
    strip_prefix = "ocp-diag-core-f47fa7b48bac87bb2848de69217163208a3f963f/apis/c++",
    urls = ["https://github.com/opencomputeproject/ocp-diag-core/archive/f47fa7b48bac87bb2848de69217163208a3f963f.zip"],  # 2023-03-22
)

load("@ocp_diag_core//ocpdiag:build_deps.bzl", "load_deps")

load_deps("ocp_diag_core")

load("@ocp_diag_core//ocpdiag:secondary_build_deps.bzl", "load_secondary_deps")

load_secondary_deps()

load("@ocp_diag_core//ocpdiag:tertiary_build_deps.bzl", "load_tertiary_deps")

load_tertiary_deps()
