#!/usr/bin/env bash
set -euo pipefail
# Select a compiler through CXX before first configure; additional CMake args may
# select a local toolchain or an already downloaded GoogleTest source tree.
for profile in asan-ubsan tsan; do
    cmake --preset "$profile" "$@"
    cmake --build --preset "$profile" --parallel "${MKA_BUILD_JOBS:-2}"
    mkdir -p "build/$profile/report"
    uname -a > "build/$profile/report/environment.txt"
    cmake --version >> "build/$profile/report/environment.txt"
    "${CXX:-c++}" --version >> "build/$profile/report/environment.txt"
    ctest --preset "$profile" --output-junit "$(pwd)/build/$profile/report/results.xml" 2>&1 | tee "build/$profile/report/ctest.log"
    ctest --preset "$profile" -V -R V1Stress 2>&1 | tee "build/$profile/report/stress.log"
done
