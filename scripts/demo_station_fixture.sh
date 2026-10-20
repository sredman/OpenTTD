#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEMO_INVOCATION_ID="run_station_$$_${RANDOM}"
DEMO_ENV_DIR="${REPO_DIR}/scripts/demo_env/${DEMO_INVOCATION_ID}"
CFG_FILE="${DEMO_ENV_DIR}/demo_isolated.cfg"
DISPLAY_NUM=":99"

XVNC_PID=""
OPENTTD_PID=""

cleanup() {
  echo "Cleaning up temporary fixture files and background processes..."
  rm -rf "${DEMO_ENV_DIR}"
  if [ -n "${OPENTTD_PID}" ] && kill -0 "${OPENTTD_PID}" 2>/dev/null; then
    kill "${OPENTTD_PID}" 2>/dev/null || true
  fi
  if [ -n "${XVNC_PID}" ]; then
    if kill -0 "${XVNC_PID}" 2>/dev/null; then
      kill "${XVNC_PID}" 2>/dev/null || true
    fi
    local lock_file="/tmp/.X${DISPLAY_NUM#:}-lock"
    if [ -f "${lock_file}" ]; then
      local lock_pid
      lock_pid="$(tr -d '[:space:]' < "${lock_file}" 2>/dev/null || true)"
      if [ "${lock_pid}" = "${XVNC_PID}" ]; then
        rm -f "${lock_file}" "/tmp/.X11-unix/X${DISPLAY_NUM#:}" 2>/dev/null || true
      fi
    fi
  fi
}
trap cleanup EXIT INT TERM

echo "=== 1. Starting isolated Xvnc on ${DISPLAY_NUM} ==="
if ! pgrep -f "Xvnc ${DISPLAY_NUM}" >/dev/null; then
  Xvnc "${DISPLAY_NUM}" -geometry 1280x800 -depth 24 -SecurityTypes None -rfbport 0 -interface 127.0.0.1 &
  STARTED_PID=$!
  sleep 1
  if kill -0 "${STARTED_PID}" 2>/dev/null; then
    XVNC_PID="${STARTED_PID}"
  else
    echo "ERROR: Failed to start Xvnc on ${DISPLAY_NUM}" >&2
    exit 1
  fi
fi

echo "=== 2. Preparing isolated environment and ephemeral scripts ==="
mkdir -p "${DEMO_ENV_DIR}/scripts"
cp "${REPO_DIR}/build-gui/regression/regression.cfg" "${CFG_FILE}"

cat << 'EOF' > "${DEMO_ENV_DIR}/scripts/autoexec.scr"
setting gui.smooth_scroll 0
setting_newgame game_creation.map_x 8
setting_newgame game_creation.map_y 8
newgame
EOF

cat << 'EOF' > "${DEMO_ENV_DIR}/scripts/game_start.scr"
setup_station_fixture
scrollto instant 140 100
zoomto 0
schedule_ticks 120 "hide_and_change_station_a"
schedule_ticks 240 "rescout_station_a"
schedule_ticks 400 "exit"
unpause
EOF

echo "=== 3. Launching OpenTTD in graphical mode on ${DISPLAY_NUM} with fixed seed -G 42 ==="
cd "${REPO_DIR}"
env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY_NUM}" \
  ./build-gui/openttd -v sdl -b 32bpp-optimized -s null -m null -x \
  -c "${CFG_FILE}" -r 1280x800 -G 42 -d console=5 -Q &
OPENTTD_PID=$!

echo "=== 4. Capturing station fixture sequence ==="
# Frame 1: At ~1.5s (before tick 120): Initial state with Station A ("Verdun Terminal") and Waypoint B ("Meuse Waypoint") under fog, unscouted
sleep 1.5
import -display "${DISPLAY_NUM}" -window root capture_station_initial.png
echo "Captured capture_station_initial.png"

# Frame 2: At ~5.5s (after tick 120, before tick 240): Station A modified and Waypoint B deleted while hidden under fog -> stale signs retained
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_station_hidden_change.png
echo "Captured capture_station_hidden_change.png"

# Frame 3: At ~9.5s (after tick 240): Aircraft rescouts tiles -> updated Station A ("Verdun Fortress Central") and removed Waypoint B
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_station_rescout.png
echo "Captured capture_station_rescout.png"

echo "Waiting for OpenTTD to exit..."
wait "${OPENTTD_PID}" || true
echo "=== Demonstration completed successfully ==="
