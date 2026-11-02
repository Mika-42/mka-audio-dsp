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
    # CXX can differ from the compiler selected by a toolchain or cached build.
    python3 - "build/$profile" >> "build/$profile/report/environment.txt" <<'PY'
import pathlib, re, subprocess, sys
build = pathlib.Path(sys.argv[1])
config = next((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
compiler = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', config.read_text()).group(1)
print(subprocess.check_output([compiler, '--version'], text=True), end='')
PY
    ctest --preset "$profile" --output-junit "$(pwd)/build/$profile/report/results.xml" 2>&1 | tee "build/$profile/report/ctest.log"
    ctest --preset "$profile" -V -R V1Stress 2>&1 | tee "build/$profile/report/stress.log"
done
