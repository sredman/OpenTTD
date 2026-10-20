#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEMO_INVOCATION_ID="run_town_$$_${RANDOM}"
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
setting gui.population_in_label 1
setting_newgame game_creation.map_x 8
setting_newgame game_creation.map_y 8
newgame
EOF

cat << 'EOF' > "${DEMO_ENV_DIR}/scripts/game_start.scr"
setup_town_fixture
scrollto instant 140 100
zoomto 0
schedule_ticks 120 "hide_and_change_town_a"
schedule_ticks 240 "rescout_town_a"
schedule_ticks 400 "exit"
unpause
EOF

echo "=== 3. Launching OpenTTD in graphical mode on ${DISPLAY_NUM} with fixed seed -G 42 ==="
cd "${REPO_DIR}"
env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY_NUM}" \
  ./build-gui/openttd -v sdl -b 32bpp-optimized -s null -m null -x \
  -c "${CFG_FILE}" -r 1280x800 -G 42 -d console=5 -Q &
OPENTTD_PID=$!

echo "=== 4. Capturing town fixture sequence ==="
# Frame 1: At ~1.5s (before tick 120): Initial state with Town A ("Verdun", pop 1,200) under fog, unscouted
sleep 1.5
import -display "${DISPLAY_NUM}" -window root capture_town_initial_a.png
echo "Captured capture_town_initial_a.png"

# Frame 2: At ~5.5s (after tick 120, before tick 240): Town A modified to "Verdun Fortress" (pop 3,500, city) while hidden under fog -> stale snapshot "Verdun (1,200)" on main map and "Verdun" on minimap
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_town_hidden_change.png
echo "Captured capture_town_hidden_change.png"

# Frame 3: At ~9.5s (after tick 240): Aircraft rescouts tile A -> discovered "Verdun Fortress" (pop 3,500, city), updated on main map and minimap
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_town_rescout.png
echo "Captured capture_town_rescout.png"

echo "Waiting for OpenTTD to exit..."
wait "${OPENTTD_PID}" || true
echo "=== Demonstration completed successfully ==="
