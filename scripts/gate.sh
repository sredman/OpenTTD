#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="${SCRIPT_DIR}/required_tests.txt"

MODE="auto"
BUILD_DIR=""
GUI_BUILD_DIR=""

# Parse command line arguments
while [ $# -gt 0 ]; do
  case "$1" in
    --mode)
      MODE="$2"
      shift 2
      ;;
    --mode=*)
      MODE="${1#*=}"
      shift 1
      ;;
    *)
      if [ -z "${BUILD_DIR}" ]; then
        BUILD_DIR="$1"
      elif [ -z "${GUI_BUILD_DIR}" ]; then
        GUI_BUILD_DIR="$1"
      fi
      shift 1
      ;;
  esac
done

BUILD_DIR="${BUILD_DIR:-build}"

if [ ! -f "${MANIFEST}" ]; then
  echo "ERROR: Required test manifest '${MANIFEST}' not found!" >&2
  exit 1
fi

run_gate_for_build() {
  local target_dir="$1"
  local target_mode="$2" # "dedicated" or "graphical"

  echo "=== [Gate] Validating Directory: ${target_dir} (Mode: ${target_mode}) ==="
  if [ ! -d "${target_dir}" ]; then
    echo "ERROR: Build directory '${target_dir}' does not exist!" >&2
    exit 1
  fi

  echo "=== [Gate] Building Required Targets (openttd, openttd_test, regression_files, regression) ==="
  cmake --build "${target_dir}" --target openttd openttd_test regression_files regression -j "$(nproc)"

  echo "=== [Gate] Verifying Built Artifacts & Fixtures ==="
  for file in "${target_dir}/openttd" "${target_dir}/openttd_test" "${target_dir}/regression/regression.cfg" "${target_dir}/game/aircraft/test.sav"; do
    if [ ! -f "${file}" ]; then
      echo "ERROR: Required artifact/fixture '${file}' is missing!" >&2
      exit 1
    fi
  done

  # Verify binary capabilities for requested mode
  local video_drivers
  video_drivers=$("${target_dir}/openttd" -h 2>&1 | grep "List of video drivers:" -A 10 || true)
  if [ "${target_mode}" = "graphical" ]; then
    if ! echo "${video_drivers}" | grep -qw "sdl"; then
      echo "ERROR: Graphical gate requested for '${target_dir}', but executable lacks SDL/GUI support!" >&2
      exit 1
    fi
  elif [ "${target_mode}" = "dedicated" ]; then
    if ! echo "${video_drivers}" | grep -qw "dedicated"; then
      echo "ERROR: Dedicated gate requested for '${target_dir}', but executable lacks dedicated support!" >&2
      exit 1
    fi
  fi

  echo "=== [Gate] Verifying Required Test Inventory in CTest ==="
  local ctest_inventory
  ctest_inventory=$(ctest --test-dir "${target_dir}" -N)
  printf '%s\n' "${ctest_inventory}" | awk -v manifest="${MANIFEST}" '
  BEGIN {
    while ((getline line < manifest) > 0) {
      gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", line)
      if (line != "") required[line] = 1
    }
    close(manifest)
  }
  /^[ \t]*Test[ \t]+#[0-9]+:[ \t]+/ {
    sub(/^[ \t]*Test[ \t]+#[0-9]+:[ \t]+/, "")
    gsub(/[ \t\r\n]+$/, "")
    found[$0] = 1
  }
  END {
    missing = 0
    for (name in required) {
      if (!(name in found)) {
        print "ERROR: Required test \x27" name "\x27 is missing from CTest inventory in target dir!" > "/dev/stderr"
        missing++
      }
    }
    if (missing > 0) {
      print "ERROR: " missing " required tests were not registered in CTest!" > "/dev/stderr"
      exit 1
    }
  }
  '

  echo "=== [Gate] Executing Test Suites via CTest (Fail-Closed) ==="
  set +e
  local ctest_output
  ctest_output=$(ctest --test-dir "${target_dir}" --output-on-failure --timeout 60 2>&1)
  local ctest_exit=$?
  set -e

  echo "${ctest_output}"

  if [ ${ctest_exit} -ne 0 ]; then
    echo "ERROR: CTest suite execution failed with exit code ${ctest_exit}!" >&2
    exit ${ctest_exit}
  fi

  if echo "${ctest_output}" | grep -E -q "(\*\*\*Skipped|\*\*\*Failed|\*\*\*Timeout|\*\*\*Exception|\*\*\*Not Run|[[:space:]]Disabled|[[:space:]]Skipped)"; then
    echo "ERROR: Test execution contained skipped, disabled, or failing tests!" >&2
    exit 1
  fi

  echo "=== [Gate] Verifying Every Required Test Succeeded ==="
  printf '%s\n' "${ctest_output}" | awk -v manifest="${MANIFEST}" '
  BEGIN {
    while ((getline line < manifest) > 0) {
      gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", line)
      if (line != "") required[line] = 1
    }
    close(manifest)
  }
  /^[ \t]*[0-9]+\/[0-9]+[ \t]+Test[ \t]+#[0-9]+:[ \t]+/ {
    sub(/^[ \t]*[0-9]+\/[0-9]+[ \t]+Test[ \t]+#[0-9]+:[ \t]+/, "")
    if (match($0, /[ \t]+\.+[ \t]+[^ \t]+[ \t]+[0-9.]+[ \t]*sec/)) {
      tail = substr($0, RSTART)
      name = substr($0, 1, RSTART - 1)
      gsub(/[ \t\r\n]+$/, "", name)
      sub(/^[ \t]+\.+[ \t]+/, "", tail)
      split(tail, parts, /[ \t]+/)
      status = parts[1]
      results[name] = status
    }
  }
  END {
    err = 0
    for (name in results) {
      if (results[name] != "Passed") {
        print "ERROR: Test \x27" name "\x27 did not pass (status: " results[name] ")!" > "/dev/stderr"
        err = 1
      }
    }
    for (name in required) {
      if (!(name in results)) {
        print "ERROR: Required test \x27" name "\x27 was not executed!" > "/dev/stderr"
        err = 1
      } else if (results[name] != "Passed") {
        print "ERROR: Required test \x27" name "\x27 failed with status: " results[name] > "/dev/stderr"
        err = 1
      }
    }
    if (err > 0) exit 1
  }
  '

  if [ "${target_mode}" = "dedicated" ]; then
    echo "=== [Gate] Running Dedicated Headless Smoke Simulation (500 ticks) ==="
    timeout 30s "${target_dir}/openttd" \
      -D \
      -x \
      -c "${target_dir}/regression/regression.cfg" \
      -g "${target_dir}/game/aircraft/test.sav" \
      -snull \
      -mnull \
      -bnull \
      -vnull:ticks=500 \
      -d script=2 \
      -Q
    echo "=== [Gate] Dedicated Smoke Simulation Passed ==="
  elif [ "${target_mode}" = "graphical" ]; then
    echo "=== [Gate] Running Graphical Client Smoke via SDL2 Dummy Backend (100 ticks) ==="
    timeout 30s env SDL_VIDEODRIVER=dummy "${target_dir}/openttd" \
      -v sdl:ticks=100 \
      -b 32bpp-anim \
      -s null \
      -m null \
      -x \
      -c "${target_dir}/regression/regression.cfg" \
      -g "${target_dir}/game/aircraft/test.sav" \
      -d script=2 \
      -Q
    echo "=== [Gate] Graphical Client Smoke Simulation Passed ==="
  fi
}

# Determine mode execution
if [ "${MODE}" = "both" ]; then
  GUI_BUILD_DIR="${GUI_BUILD_DIR:-build-gui}"
  run_gate_for_build "${BUILD_DIR}" "dedicated"
  run_gate_for_build "${GUI_BUILD_DIR}" "graphical"
elif [ "${MODE}" = "dedicated" ]; then
  run_gate_for_build "${BUILD_DIR}" "dedicated"
elif [ "${MODE}" = "graphical" ]; then
  run_gate_for_build "${BUILD_DIR}" "graphical"
elif [ "${MODE}" = "auto" ]; then
  is_dedicated="false"
  if [ -f "${BUILD_DIR}/CMakeCache.txt" ] && grep -q "OPTION_DEDICATED:BOOL=ON" "${BUILD_DIR}/CMakeCache.txt"; then
    is_dedicated="true"
  elif [ -x "${BUILD_DIR}/openttd" ] && ! "${BUILD_DIR}/openttd" -h 2>&1 | grep "List of video drivers:" -A 10 | grep -qw "sdl"; then
    is_dedicated="true"
  fi

  if [ "${is_dedicated}" = "true" ]; then
    run_gate_for_build "${BUILD_DIR}" "dedicated"
    if [ -n "${GUI_BUILD_DIR}" ] && [ -d "${GUI_BUILD_DIR}" ]; then
      run_gate_for_build "${GUI_BUILD_DIR}" "graphical"
    fi
  else
    run_gate_for_build "${BUILD_DIR}" "graphical"
  fi
else
  echo "ERROR: Unknown mode '${MODE}'. Choose dedicated, graphical, both, or auto." >&2
  exit 1
fi

echo "=== [Gate] ALL CHECKS PASSED FOR REQUESTED MODE (${MODE}) ==="
