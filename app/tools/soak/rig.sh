#!/bin/sh
# The shell half of the soak harness: everything that wants tools/localcat's
# work directory, its throwaway CA and its two builds. run.py calls this and
# nothing else; the knowledge of where the memory stick is and which EBOOT is
# on it stays in one place, which is common.sh.
#
#   rig.sh setup        certs, the mock site, the db state, the local build
#   rig.sh plant        the mock site and db state again, nothing built
#   rig.sh clean        the mock's records and directories off the stick
#   rig.sh serve        the mock site over TLS, as a user unit
#   rig.sh stop         that unit down again
#   rig.sh build-local  the client pointed at the loopback catalog
#   rig.sh build-real   the client pointed at the published one
#   rig.sh paths        WORK and MS, one per line, for run.py to read
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$HERE/../.." && pwd)"
REPO="$(cd "$APP/.." && pwd)"
MS="$HOME/.var/app/org.ppsspp.PPSSPP/config/ppsspp"
. "$APP/tools/localcat/common.sh"

# A port and a unit of the soak's own. The work directory, the CA and the
# EBOOT are already private -- they live under this worktree's app/ -- but
# port 8443 and the unit called pspdx-mock-server are not: dev/start --mock
# takes them, and a dev/start without --mock stops that unit, which would
# pull the catalog out from under a campaign halfway through. So the soak
# serves its own catalog somewhere else and leaves the desk's alone.
LOCALCAT_PORT=8444
LOCALCAT_URL="https://127.0.0.1:$LOCALCAT_PORT/catalog.json"
SOAK_UNIT=pspdx-soak-server

soak_stop_server() { systemctl --user stop "$SOAK_UNIT" 2>/dev/null || true; }

soak_start_server() {
	soak_stop_server
	systemd-run --user --collect --quiet --unit="$SOAK_UNIT" \
		python3 "$LOCALCAT/serve.py" "$WORK/mock-site" "$WORK/srv.crt" \
		"$WORK/srv.key" "$LOCALCAT_PORT"
}

mock() {
	python3 "$REPO/dev/mock-catalog" "$1" --work "$WORK" --ms "$MS" --url "$LOCALCAT_URL"
}

case "$1" in
	setup)
		localcat_certs
		mock make
		localcat_build_local
		;;
	plant)
		# make() removes and rewrites the whole site, so a server left
		# running has its working directory deleted under it and answers
		# every request with a closed connection. The two go together.
		mock make
		soak_start_server
		;;
	idle)
		# One emulator at a time. Two write the same PSPDX.LOG, the same
		# PSPDX1.BMP and the same db records, and a rig that let a second
		# one run would be reading somebody else's session. Nothing here
		# ends that session: an emulator the user opened is theirs to
		# close, and a campaign that finds one waits and says so. Only
		# what a run of this rig started itself is its own to stop, and
		# run-ppsspp.sh does that when its seconds are up.
		if pgrep -x PPSSPPSDL >/dev/null 2>&1; then echo busy; else echo idle; fi
		;;
	clean)   mock clean ;;
	serve)   soak_start_server ;;
	stop)    soak_stop_server ;;
	build-local) localcat_build_local ;;
	build-real)  localcat_build_real ;;
	paths)   printf '%s\n%s\n%s\n' "$WORK" "$MS" "$APP" ;;
	*) echo "rig.sh: unknown command ${1:-}" >&2; exit 2 ;;
esac
