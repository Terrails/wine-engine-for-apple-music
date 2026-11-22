#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Called by `make lab` (wrapper/Makefile): generate <builddir>/iids_gen.h from the app's own
# metadata (AMP.*.winmd + Microsoft.UI.Xaml.winmd) with tools/geniids.py, or remove it when the
# metadata is not available, in which case lab.c builds with probe_iids() disabled.
# Looks in WINMD_DIR, else the installed app in AM_PREFIX, else the highest version (sort -V) in
# $AM_CACHE_DIR/msix/ (unfinished <version>.tmp unpacks are skipped).
# <builddir>/iids.stamp is rewritten only when the result changes, so lab.o is rebuilt only then.
# usage: dev/lab/iids.sh <builddir>
set -e
OUT="$(cd "$1" && pwd)"
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
. "$ROOT/bin/amd-env.sh"

has_winmd() { [ -f "$1/AMP.Services.winmd" ]; }
SRC=
if [ -n "$WINMD_DIR" ]; then
    has_winmd "$WINMD_DIR" && SRC="$WINMD_DIR" ||
        echo "lab: WINMD_DIR=$WINMD_DIR has no AMP.Services.winmd" >&2
elif has_winmd "$APPDIR"; then
    SRC="$APPDIR"
else
    SRC="$(for d in "$AM_CACHE_DIR"/msix/*/; do
        d="${d%/}"
        case "$d" in *.tmp) continue ;; esac
        if has_winmd "$d"; then printf '%s\n' "$d"; fi
    done | sort -V | tail -n 1)"
fi

if [ -n "$SRC" ] && "$AM_PYTHON" -I -c 'import dnfile' 2>/dev/null; then
    "$AM_PYTHON" -I "$ROOT/tools/geniids.py" "$OUT/iids_gen.h.new" "$SRC" >/dev/null
    if cmp -s "$OUT/iids_gen.h.new" "$OUT/iids_gen.h"; then
        rm -f "$OUT/iids_gen.h.new"
    else
        mv -f "$OUT/iids_gen.h.new" "$OUT/iids_gen.h"
    fi
    STATE="$(sha256sum "$OUT/iids_gen.h" | cut -d' ' -f1)"
    echo "lab: interface names from $SRC" >&2
else
    if [ -n "$SRC" ]; then
        echo "lab: $AM_PYTHON has no dnfile (pip install -r requirements.txt): probe_iids disabled" >&2
    else
        echo "lab: no app metadata (set WINMD_DIR or run setup-prefix.sh): probe_iids disabled" >&2
    fi
    rm -f "$OUT/iids_gen.h"
    STATE=none
fi
[ "$(cat "$OUT/iids.stamp" 2>/dev/null)" = "$STATE" ] || printf '%s\n' "$STATE" > "$OUT/iids.stamp"
