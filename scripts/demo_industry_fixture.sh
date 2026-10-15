#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEMO_INVOCATION_ID="run_industry_$$_${RANDOM}"
DEMO_ENV_DIR="${REPO_DIR}/scripts/demo_env/${DEMO_INVOCATION_ID}"
CFG_FILE="${DEMO_ENV_DIR}/demo_isolated.cfg"
DISPLAY_NUM=":99"

XVNC_PID=""
OPENTTD_PID=""

cleanup() {
  echo "Cleaning up temporary fixture files and background processes..."
  rm -rf "${DEMO_ENV_DIR}"
  if [ -n "${OPENTTD_PID}" ]; then kill "${OPENTTD_PID}" 2>/dev/null || true; fi
  if [ -n "${XVNC_PID}" ]; then
    kill "${XVNC_PID}" 2>/dev/null || true
    rm -f "/tmp/.X${DISPLAY_NUM#:}-lock" "/tmp/.X11-unix/X${DISPLAY_NUM#:}" 2>/dev/null || true
  fi
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
setup_industry_fixture
scrollto instant 140 100
zoomto 0
schedule_ticks 60 "hide_and_delete_industry_a"
schedule_ticks 180 "observe_industry_b"
schedule_ticks 300 "partial_rescout_industry_a"
schedule_ticks 420 "rescout_industry_a"
schedule_ticks 660 "exit"
unpause
EOF

echo "=== 3. Launching OpenTTD in graphical mode on ${DISPLAY_NUM} with fixed seed -G 42 ==="
cd "${REPO_DIR}"
env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY_NUM}" \
  ./build-gui/openttd -v sdl -b 32bpp-optimized -s null -m null -x \
  -c "${CFG_FILE}" -r 1280x800 -G 42 -d console=5 -Q &
OPENTTD_PID=$!

echo "=== 4. Capturing industry fixture sequence ==="
# Frame 1: After tick 60, Industry A hidden and deleted from live pool -> remembered stale snapshot on main map and Industries minimap
sleep 3.5
import -display "${DISPLAY_NUM}" -window root capture_industry_stale_a.png
echo "Captured capture_industry_stale_a.png"

# Frame 2: After tick 180, Industry B created reusing ID and observed -> A still unchanged stale snapshot, B observed
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_industry_observed_b.png
echo "Captured capture_industry_observed_b.png"

# Frame 3: After tick 300, Aircraft partially scouts tile A (140, 100) -> tile A cleared, tile A2 (141, 100) remains remembered
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_industry_partial_a.png
echo "Captured capture_industry_partial_a.png"

# Frame 4: After tick 420, Aircraft scouts all of A -> tile A and A2 cleared, Coal Mine removed on minimap, B remains
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_industry_rescout_a.png
echo "Captured capture_industry_rescout_a.png"

echo "Waiting for OpenTTD to exit..."
wait "${OPENTTD_PID}" 2>/dev/null || true
echo "=== Industry fixture run complete! ==="
