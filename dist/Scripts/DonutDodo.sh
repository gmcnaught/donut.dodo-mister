#!/bin/bash
#
# Donut Dodo — MiSTer Scripts-menu entry: loads the DonutDodo core and starts
# the game.
#
# With Scripts -> DonutDodo_CoresMenu turned on (MiSTer.ini [DonutDodo]
# main=...), loading the core from the core list also starts the game, and this
# entry only loads the core. Otherwise this entry starts games/DonutDodo/launch.sh
# itself once the core is up.

GAMEDIR=/media/fat/games/DonutDodo
HANDLER="$GAMEDIR/launch.sh"
WRAPPER="$GAMEDIR/MiSTer_DonutDodo"
INI=/media/fat/MiSTer.ini
RBF_GLOB="/media/fat/_Other/DonutDodo_*.rbf"
CORENAME="DonutDodo"
LOGDIR=/media/fat/logs/DonutDodo
CORE_WAIT_S=30

mkdir -p "$LOGDIR"
echo "Donut Dodo: loading core and starting the engine..."
echo "log: $LOGDIR/launch.log"
exec >> "$LOGDIR/launch.log" 2>&1
echo "=== $(date) Scripts entry (pid $$)"

die() { echo "launcher: $*"; exit 1; }

[ -f "$HANDLER" ] || die "handler not found: $HANDLER"
[ -f "$GAMEDIR/gamedata/DonutDodo.pck" ] || die "missing $GAMEDIR/gamedata/DonutDodo.pck — copy it from your itch.io download (see README.md)"
# update_all and zip extraction don't preserve the execute bit.
chmod +x "$HANDLER" "$WRAPPER" "$GAMEDIR/frt_3.5.2" 2>/dev/null

main_on() { [ -x "$WRAPPER" ] && grep -q "^main=$WRAPPER" "$INI" 2>/dev/null; }
launcher_running() { ps -o args | grep -q '[D]onutDodo/launch.sh'; }

# --- core ------------------------------------------------------------------------
if [ "$(cat /tmp/CORENAME 2>/dev/null)" != "$CORENAME" ]; then
	RBF="$(ls -t $RBF_GLOB 2>/dev/null | head -1)"
	[ -n "$RBF" ] || die "no RBF matching $RBF_GLOB"
	[ -p /dev/MiSTer_cmd ] || die "/dev/MiSTer_cmd missing — is MiSTer running?"
	echo "launcher: load_core $RBF"
	echo "load_core $RBF" > /dev/MiSTer_cmd
	if main_on; then
		echo "launcher: main= is on — MiSTer_DonutDodo starts the game"
		exit 0
	fi
	waited=0
	while [ "$(cat /tmp/CORENAME 2>/dev/null)" != "$CORENAME" ]; do
		sleep 1
		waited=$((waited + 1))
		[ "$waited" -ge "$CORE_WAIT_S" ] && die "core did not come up within ${CORE_WAIT_S}s (CORENAME='$(cat /tmp/CORENAME 2>/dev/null)')"
	done
	echo "launcher: core up after ${waited}s"
fi

# Core already up: start the game unless it is running.
launcher_running && { echo "launcher: launch.sh already running"; exit 0; }
# Detach: the Scripts console returns to the menu while the game runs.
setsid "$HANDLER" < /dev/null >> "$LOGDIR/launch.log" 2>&1 &
echo "launcher: handler started (pid $!)"
exit 0
