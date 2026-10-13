#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"
GUI_BUILD_DIR="${2:-build-gui}"

echo "=== [Gate] Validating Build Directory: ${BUILD_DIR} ==="
if [ ! -d "${BUILD_DIR}" ]; then
  echo "ERROR: Build directory '${BUILD_DIR}' does not exist." >&2
  exit 1
fi

echo "=== [Gate] Building Required Targets (openttd, openttd_test, regression_files, regression) ==="
cmake --build "${BUILD_DIR}" --target openttd openttd_test regression_files regression -j "$(nproc)"

echo "=== [Gate] Verifying Built Artifacts & Fixtures ==="
for file in "${BUILD_DIR}/openttd" "${BUILD_DIR}/openttd_test" "${BUILD_DIR}/regression/regression.cfg" "${BUILD_DIR}/game/aircraft/test.sav"; do
  if [ ! -f "${file}" ]; then
    echo "ERROR: Required artifact/fixture '${file}' is missing!" >&2
    exit 1
  fi
done

echo "=== [Gate] Running Test Suites via CTest ==="
CTEST_OUTPUT=$(ctest --test-dir "${BUILD_DIR}" --output-on-failure --timeout 60)
echo "${CTEST_OUTPUT}"

# Fail-closed check: verify 100% tests passed and at least 99 tests ran
if ! echo "${CTEST_OUTPUT}" | grep -q "100% tests passed, 0 tests failed out of"; then
  echo "ERROR: Test suite failure detected in ctest!" >&2
  exit 1
fi

TOTAL_TESTS=$(echo "${CTEST_OUTPUT}" | grep -o "out of [0-9]*" | awk '{print $3}')
if [ -z "${TOTAL_TESTS}" ] || [ "${TOTAL_TESTS}" -lt 99 ]; then
  echo "ERROR: Expected at least 99 tests to run, but found ${TOTAL_TESTS:-0}!" >&2
  exit 1
fi

echo "=== [Gate] Running Headless Dedicated Smoke Simulation (500 ticks) ==="
timeout 30s "${BUILD_DIR}/openttd" \
  -x \
  -c "${BUILD_DIR}/regression/regression.cfg" \
  -g "${BUILD_DIR}/game/aircraft/test.sav" \
  -snull \
  -mnull \
  -bnull \
  -vnull:ticks=500 \
  -d script=2 \
  -Q

if [ -x "${GUI_BUILD_DIR}/openttd" ]; then
  echo "=== [Gate] Running Bounded Graphical Client Smoke (500 ticks) ==="
  timeout 30s "${GUI_BUILD_DIR}/openttd" \
    -x \
    -c "${BUILD_DIR}/regression/regression.cfg" \
    -g "${BUILD_DIR}/game/aircraft/test.sav" \
    -snull \
    -mnull \
    -bnull \
    -vnull:ticks=500 \
    -Q
fi

echo "=== [Gate] ALL CHECKS PASSED (${TOTAL_TESTS} tests + bounded smoke) ==="
