#!/bin/sh
# Runs EBOOT.PBP under the PPSSPP flatpak, without needing a visible desktop,
# and leaves PSPDX.LOG and PSPDX.BMP (as shot.png) next to this script.
#
#   sh app/run-ppsspp.sh [seconds] [--sweep] [--keys FILE] [--slow [KB/s]]
#
# --keys FILE scripts input: one "<ms> <key>" per line, counted from the
# moment the catalog is up; keys are up, down, cross, circle, square,
# triangle, start, select and shot, the last of which leaves a settled
# screenshot as shot1.png next to shot.png.
#
# --slow [KB/s] paces the client's network down to a PSP-1004's, 180 KB/s by
# default, which is what its 802.11b radio and TCP stack were measured at.
#
# By default the emulator's stick gets a fixed test seed, so the client does
# what it does on a PSP after its first run: load the seed, skip the sweep,
# and be at the catalog a few seconds in. The seed is the string below and
# obviously not entropy; nothing from a rig run is fit to sign anything.
#
# --sweep replays testdata/sweep.trace through the entropy screen instead,
# for working on that screen. That trace is the first six seconds of
# sweep-full.trace, so the screen is over in six seconds; it was cut at the
# 128-bit mark when a field alone earned a bit, and now that only a turn does
# it ends with the bar a few bits along, which a replay may. The full one is
# what the rate in logic/entropy.h was measured on. A replayed sweep never writes a seed -- the
# client refuses, since replayed input is not entropy either -- so the next
# default run seeds itself again.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MS="$HOME/.var/app/org.ppsspp.PPSSPP/config/ppsspp"
SECS=25
SWEEP=0
KEYS=""
SLOW=""
while [ $# -gt 0 ]; do
	case "$1" in
		--sweep) SWEEP=1 ;;
		--keys) KEYS="$2"; shift ;;
		--slow) SLOW="${2:-180}"; case "$SLOW" in ''|*[!0-9]*) SLOW=180 ;; *) shift ;; esac ;;
		*) SECS="$1" ;;
	esac
	shift
done

mkdir -p "$MS/PSP/GAME/pspdx"
cp "$HERE/EBOOT.PBP" "$MS/PSP/GAME/pspdx/"

# PPSSPP ships the PSP system fonts but does not mount flash0 for the guest,
# so put one where the client's fallback looks. Test rig only: on hardware the
# font comes out of the PSP's own firmware and nothing is copied.
FONTS="$(flatpak info --show-location org.ppsspp.PPSSPP 2>/dev/null)/files/share/ppsspp/assets/flash0/font"
if [ -f "$FONTS/ltn8.pgf" ]; then
	mkdir -p "$MS/PSP/PSPDX/font"
	cp "$FONTS/ltn8.pgf" "$MS/PSP/PSPDX/font/ltn8.pgf"
fi

# Clips the catalog repo holds but the published catalog does not link yet
# go straight into the client's cache, which it consults before any URL.
# That is how the player gets exercised ahead of a deploy.
for clip in "$HERE"/../catalog/apps/*/video.mp4; do
	[ -f "$clip" ] || continue
	id="$(basename "$(dirname "$clip")")"
	mkdir -p "$MS/PSP/PSPDX/cache"
	cp "$clip" "$MS/PSP/PSPDX/cache/$id.mp4"
done

if [ "$SWEEP" = 1 ]; then
	cp "$HERE/testdata/sweep.trace" "$MS/PSPDX.TRACE"
	touch "$MS/PSPDX.REPLAY"
	rm -f "$MS/PSPDX.SEED"
else
	rm -f "$MS/PSPDX.REPLAY"
	# 20 bytes, the pool size. Only written when missing: the client ratchets
	# the file forward on every run, and a rig that kept resetting it would
	# hide a bug in that.
	[ -f "$MS/PSPDX.SEED" ] || printf 'PSPDX-TEST-SEED-0000' >"$MS/PSPDX.SEED"
fi
rm -f "$MS/PSPDX.LOG" "$MS/PSPDX.BMP" "$MS/PSPDX1.BMP" "$MS/PSPDX.BENCH" "$MS/PSPDX.KEYS" \
      "$MS/PSPDX.SLOW"
[ -n "$KEYS" ] && cp "$KEYS" "$MS/PSPDX.KEYS"
# The emulator borrows the host's network; a PSP-1004 has 802.11b and its own
# TCP stack, which together managed about 180 KB/s. --slow holds the client to
# that, so a film takes as long to arrive here as it does on the hardware.
[ -n "$SLOW" ] && printf '%s' "$SLOW" >"$MS/PSPDX.SLOW"

# Native: the emulator renders the PSP's own 480x272 and only the window
# scales it, so what is on screen is what a PSP shows, pixel for pixel.
# The ini is rewritten by a running emulator on exit; a rig run that
# overlaps one loses this, which is harmless for a rig.
INI="$MS/PSP/SYSTEM/ppsspp.ini"
[ -f "$INI" ] && sed -i 's/^InternalResolution = .*/InternalResolution = 1/' "$INI"

# In its own session, so that the kill below reaches the emulator inside
# the flatpak sandbox and not only the launcher: an instance that survives
# keeps writing the same files as the next run, and two runs then share one
# log.
# --nosocket=pulseaudio: a rig run has no ear on it. The client's own stream
# is checked through the emulator's DumpAudio, not through the speakers.
SDL_VIDEODRIVER=wayland setsid flatpak run --socket=wayland --share=network \
  --nosocket=pulseaudio \
  --filesystem="$MS" org.ppsspp.PPSSPP --fullscreen=0 \
  "$MS/PSP/GAME/pspdx/EBOOT.PBP" >"$HERE/ppsspp.out" 2>&1 &
PID=$!
sleep "$SECS"
kill -- -"$PID" 2>/dev/null || kill "$PID" 2>/dev/null || true
sleep 2

cp "$MS/PSPDX.LOG" "$HERE/PSPDX.LOG" 2>/dev/null || echo "no log written"
[ -f "$MS/PSPDX.BMP" ] && magick "$MS/PSPDX.BMP" -scale 200% "$HERE/shot.png"
rm -f "$HERE/shot1.png"
[ -f "$MS/PSPDX1.BMP" ] && magick "$MS/PSPDX1.BMP" -scale 200% "$HERE/shot1.png"
cat "$HERE/PSPDX.LOG" 2>/dev/null
