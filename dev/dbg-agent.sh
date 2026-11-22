#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run AMPLibraryAgent under winedbg and print a backtrace at the first unhandled
# (second-chance) exception. GUI-less crashes otherwise lose winedbg's output.
# usage: dev/dbg-agent.sh <logname> [exe=AMPLibraryAgent.exe]      log: $AM_LOG_DIR/<logname>.log
#   DBG=<WINEDEBUG> (default err), T=<seconds> (default 90)
# The service must not be running (bin/amctl quit).
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$ROOT/bin/amd-env.sh"
[ -n "$1" ] || { sed -n '5,6p' "$(readlink -f "$0")"; exit 2; }
if am_busy; then echo "the service or the agent is running; stop it first: bin/amctl quit" >&2; exit 1; fi
am_need_app
am_logs
cd "$APPDIR" || exit 1
printf 'set $BreakOnFirstChance=0\ncont\nbt\ninfo share\nquit\n' | \
    WINEDEBUG=${DBG:-err} timeout "${T:-90}" winedbg "C:\\Program Files\\AppleMusic\\${2:-AMPLibraryAgent.exe}" -Embedding > "$LOGS/$1.log" 2>&1
wineserver -k 2>/dev/null
grep -A25 "^Backtrace" "$LOGS/$1.log" | head -30
