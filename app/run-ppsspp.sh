#!/bin/sh
# Runs EBOOT.PBP under the PPSSPP flatpak, without needing a visible desktop,
# and leaves PSPDX.LOG and PSPDX.BMP (as shot.png) next to this script.
#
#   sh app/run-ppsspp.sh [seconds]      default 45
#
# The sweep is replayed from testdata/sweep.trace so no one has to move a
# stick; that also means the entropy of such a run is NOT real.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MS="$HOME/.var/app/org.ppsspp.PPSSPP/config/ppsspp"
SECS="${1:-45}"

mkdir -p "$MS/PSP/GAME/pspdx"
cp "$HERE/EBOOT.PBP" "$MS/PSP/GAME/pspdx/"
cp "$HERE/testdata/sweep.trace" "$MS/PSPDX.TRACE"

# PPSSPP ships the PSP system fonts but does not mount flash0 for the guest,
# so put one where the client's fallback looks. Test rig only: on hardware the
# font comes out of the PSP's own firmware and nothing is copied.
FONTS="$(flatpak info --show-location org.ppsspp.PPSSPP 2>/dev/null)/files/share/ppsspp/assets/flash0/font"
if [ -f "$FONTS/ltn8.pgf" ]; then
	mkdir -p "$MS/PSP/PSPDX/font"
	cp "$FONTS/ltn8.pgf" "$MS/PSP/PSPDX/font/ltn8.pgf"
fi
touch "$MS/PSPDX.REPLAY"
rm -f "$MS/PSPDX.LOG" "$MS/PSPDX.BMP"

SDL_VIDEODRIVER=wayland flatpak run --socket=wayland --share=network \
  --filesystem="$MS" org.ppsspp.PPSSPP --fullscreen=0 \
  "$MS/PSP/GAME/pspdx/EBOOT.PBP" >"$HERE/ppsspp.out" 2>&1 &
PID=$!
sleep "$SECS"
kill $PID 2>/dev/null || true
sleep 2

cp "$MS/PSPDX.LOG" "$HERE/PSPDX.LOG" 2>/dev/null || echo "no log written"
[ -f "$MS/PSPDX.BMP" ] && magick "$MS/PSPDX.BMP" -scale 200% "$HERE/shot.png"
cat "$HERE/PSPDX.LOG" 2>/dev/null
