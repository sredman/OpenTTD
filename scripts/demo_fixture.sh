#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEMO_INVOCATION_ID="run_$$_${RANDOM}"
DEMO_ENV_DIR="${REPO_DIR}/scripts/demo_env/${DEMO_INVOCATION_ID}"
CFG_FILE="${DEMO_ENV_DIR}/demo_isolated.cfg"
DISPLAY_NUM=":99"

XVNC_PID=""
OPENTTD_PID=""

cleanup() {
  echo "Cleaning up temporary fixture files and background processes..."
  rm -rf "${DEMO_ENV_DIR}"
  if [ -n "${OPENTTD_PID}" ]; then kill "${OPENTTD_PID}" 2>/dev/null || true; fi
  if [ -n "${XVNC_PID}" ]; then kill "${XVNC_PID}" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

echo "=== 1. Starting isolated Xvnc on ${DISPLAY_NUM} ==="
if ! pgrep -f "Xvnc ${DISPLAY_NUM}" >/dev/null; then
  Xvnc "${DISPLAY_NUM}" -geometry 1280x800 -depth 24 -SecurityTypes None -rfbport 0 -interface 127.0.0.1 &
  XVNC_PID=$!
  sleep 1
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
setup_military_scenario
scrollto instant 140 100
zoomto 0
print_military_status
schedule_ticks 50 "relocate_military_aircraft 140 100"
schedule_ticks 60 "print_military_status"
schedule_ticks 300 "relocate_military_aircraft 180 120"
schedule_ticks 310 "print_military_status"
schedule_ticks 450 "exit"
unpause
EOF

echo "=== 3. Launching OpenTTD in graphical mode on ${DISPLAY_NUM} with fixed seed -G 42 ==="
cd "${REPO_DIR}"
env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY_NUM}" \
  ./build-gui/openttd -v sdl -b 32bpp-optimized -s null -m null -x \
  -c "${CFG_FILE}" -r 1280x800 -G 42 -d console=5 -Q &
OPENTTD_PID=$!

echo "=== 4. Capturing fixed-camera visible and absent progression ==="
# t1: Aircraft present at (140, 100), tile scouted and enemy marker visibly rendered
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_enemy_visible.png
echo "Captured capture_enemy_visible.png"

# t2: Aircraft relocated to (180, 120), tile 140 becomes stale snapshot, enemy marker absent on fixed camera
sleep 8.0
import -display "${DISPLAY_NUM}" -window root capture_enemy_absent.png
echo "Captured capture_enemy_absent.png"

echo "Waiting for OpenTTD to exit..."
wait "${OPENTTD_PID}" 2>/dev/null || true
echo "=== Fixture run complete! ==="
