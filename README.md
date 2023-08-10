# Stress App Test Diagnostic

This repository contains a version of the [Stress App Test](https://github.com/stressapptest/stressapptest) that conforms to the [OCP diagnostic output specification](https://github.com/opencomputeproject/ocp-diag-core/tree/main/json_spec) using the [C++ API](https://github.com/opencomputeproject/ocp-diag-core-cpp).

## Building the Diagnostic

### Dependecies

Install `bazelisk`:

```
npm install -g @bazel/bazelisk
```
(instructions for `npm` installation at [docs.npmjs.com](https://docs.npmjs.com/downloading-and-installing-node-js-and-npm/))

Install `autoconf` dependencies. The following names are usually present in most package managers on linux:
- autoconf
- automake

Note that `autoconf` is currently only required for the code dependencies on `stressapptest_config.h`, which is an artifact of the code import from [stressapptest](https://github.com/stressapptest/stressapptest). The code in this repo is otherwise built with `bazel`.

### Building the binary package

Now you should be able to navigate to the top level of this repo and run the following command to build the SAT binary:

```
bazel build //...
```

Then pick the output binary artifact from `bazel-bin/src/ocp_diag_sat_x86_64`.
