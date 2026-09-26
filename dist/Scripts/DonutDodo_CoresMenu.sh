#!/bin/bash
#
# Donut Dodo — Cores-menu toggle (from maldita.castilla-mister's
# MalditaCastilla_CoresMenu.sh). Run once to make loading DonutDodo from the
# core list (_Other) start the game; run again to undo.
#
# It adds this to /media/fat/MiSTer.ini (backed up first):
#
#   [DonutDodo]
#   main=/media/fat/games/DonutDodo/MiSTer_DonutDodo
#
# main= is MiSTer's per-core setting for "run this Main binary instead of
# /media/fat/MiSTer while this core is loaded". MiSTer_DonutDodo is upstream
# Main_MiSTer plus one call that starts games/DonutDodo/launch.sh once the core
# is up. Loading any other core switches back to the normal /media/fat/MiSTer,
# so MiSTer updates keep applying everywhere else. If MiSTer_DonutDodo is
# missing, MiSTer ignores the line and loads the core without starting the game.
#
# Scripts -> DonutDodo starts the game with or without this.

CORENAME="DonutDodo"
# The DD_* overrides let a host test the MiSTer.ini edit; MiSTer never sets them.
WRAPPER="${DD_WRAPPER:-/media/fat/games/DonutDodo/MiSTer_DonutDodo}"
INI="${DD_INI:-/media/fat/MiSTer.ini}"
MAIN_LINE="main=$WRAPPER"
# Only the hooked build contains this path; a stock Main_MiSTer renamed to
# MiSTer_DonutDodo would load the core and never start the game.
WRAPPER_MARK="/media/fat/games/DonutDodo/launch.sh"

die() { echo "$*" >&2; exit 1; }
armed() { grep -q "^$MAIN_LINE" "$INI" 2>/dev/null; }
backup_ini() { cp "$INI" "$INI.bak.$(date +%s)" || die "could not back up $INI — not editing it"; }

# sed to a temp file + rename (portable across busybox and BSD sed).
ini_sed() {
    local tmp="$INI.tmp.$$"
    sed "$1" "$INI" > "$tmp" && mv "$tmp" "$INI" && return 0
    rm -f "$tmp"; die "edit failed — $INI is unchanged except for the backup"
}
# Insert after the first [DonutDodo] header only (busybox sed has no 0,/re/).
ini_insert_in_section() {
    local tmp="$INI.tmp.$$"
    awk -v line="$1" -v sec="[$CORENAME]" '{ print } !ins && $0 == sec { print line; ins = 1 }' "$INI" > "$tmp" \
        && mv "$tmp" "$INI" && return 0
    rm -f "$tmp"; die "edit failed — $INI is unchanged except for the backup"
}

if armed; then
    echo "Cores-menu start is currently ON. Turning it off."
    backup_ini
    ini_sed "s|^$MAIN_LINE.*|;$MAIN_LINE  ; disabled by DonutDodo_CoresMenu|"
    echo "Done. Loading the core no longer starts the game; use Scripts -> DonutDodo."
    exit 0
fi

[ -f "$WRAPPER" ] || die "not found: $WRAPPER — extract the release zip over /media/fat again."
grep -q "$WRAPPER_MARK" "$WRAPPER" || die "$WRAPPER is not the Donut Dodo build of MiSTer — not using it."
[ -x "$WRAPPER" ] || chmod +x "$WRAPPER" || die "could not make $WRAPPER executable"
[ -f "$INI" ] || { echo "note: $INI did not exist — creating it with just this section."; : > "$INI" || die "could not create $INI"; }

echo "Turning Cores-menu start on."
backup_ini
# Re-enable a commented line, else add to an existing section, else append a
# new section — never a second [DonutDodo] header.
if grep -q "^;$MAIN_LINE" "$INI"; then
    ini_sed "s|^;$MAIN_LINE.*|$MAIN_LINE|"
elif grep -q "^\[$CORENAME\]$" "$INI"; then
    ini_insert_in_section "$MAIN_LINE"
else
    printf '\n[%s]\n%s\n' "$CORENAME" "$MAIN_LINE" >> "$INI" || die "could not append to $INI"
fi
echo "Done. Loading DonutDodo from the core list (_Other) now starts the game."
echo "Run this entry again to undo. Backups: $INI.bak.*"
