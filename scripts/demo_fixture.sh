#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CFG_FILE="${REPO_DIR}/../test_env/test_isolated.cfg"
DISPLAY_NUM=":99"

XVNC_PID=""
OPENTTD_PID=""

cleanup() {
  echo "Cleaning up processes..."
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

echo "=== 2. Preparing game scripts for continuous fixture demonstration ==="
cat << 'EOF' > "${REPO_DIR}/scripts/autoexec.scr"
setting gui.smooth_scroll 0
setting_newgame game_creation.map_x 8
setting_newgame game_creation.map_y 8
newgame
EOF

cat << 'EOF' > "${REPO_DIR}/scripts/game_start.scr"
setup_military_scenario
order_military_aircraft 140 100
scrollto instant 140 100
zoomto 0
print_military_status
schedule_ticks 50 "follow_military_aircraft"
schedule_ticks 50 "print_military_status"
schedule_ticks 150 "print_military_status"
schedule_ticks 300 "print_military_status"
schedule_ticks 450 "print_military_status"
schedule_ticks 620 "scrollto instant 140 100"
schedule_ticks 640 "print_military_status"
schedule_ticks 670 "order_military_aircraft 180 100"
schedule_ticks 720 "scrollto instant 140 100"
schedule_ticks 840 "print_military_status"
schedule_ticks 1200 "exit"
unpause
EOF

echo "=== 3. Launching OpenTTD in graphical mode on ${DISPLAY_NUM} with fixed seed -G 42 ==="
cd "${REPO_DIR}"
env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY_NUM}" \
  ./build-gui/openttd -v sdl -b 32bpp-optimized -s null -m null -x \
  -c "${CFG_FILE}" -r 1280x800 -G 42 -d console=5 -Q &
OPENTTD_PID=$!

echo "=== 4. Capturing continuous gameplay progression ==="
# t0: Enemy marker at (140, 100) under never-scouted fog
sleep 1.2
import -display "${DISPLAY_NUM}" -window root capture_enemy_hidden.png
echo "Captured capture_enemy_hidden.png"

# t1: Aircraft in ordinary flight at tile ~110 (viewport following aircraft)
sleep 3.5
import -display "${DISPLAY_NUM}" -window root capture_aircraft_mov1.png
echo "Captured capture_aircraft_mov1.png"

# t2: Aircraft in ordinary flight at tile ~119 (viewport following aircraft)
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_aircraft_mov2.png
echo "Captured capture_aircraft_mov2.png"

# t3: Aircraft in ordinary flight at tile ~129 (viewport following aircraft)
sleep 4.0
import -display "${DISPLAY_NUM}" -window root capture_aircraft_mov3.png
echo "Captured capture_aircraft_mov3.png"

# t4: Aircraft arrived at (140, 100), tile scouted and currently visible
sleep 5.0
import -display "${DISPLAY_NUM}" -window root capture_enemy_visible.png
echo "Captured capture_enemy_visible.png"

# t5: Aircraft ordered to (180, 100), flew away past 5-tile radius; tile 140 has stale snapshot, enemy absent
sleep 6.0
import -display "${DISPLAY_NUM}" -window root capture_enemy_absent.png
echo "Captured capture_enemy_absent.png"

echo "Waiting for OpenTTD to exit..."
wait "${OPENTTD_PID}" 2>/dev/null || true
echo "=== Fixture run complete! ==="
