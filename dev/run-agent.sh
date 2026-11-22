#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run AMPLibraryAgent alone in the prefix (AM_PREFIX, see bin/amd-env.sh).
# usage: dev/run-agent.sh <logname> [seconds=60] [WINEDEBUG]
#   log goes to $AM_LOG_DIR/<logname>.log
#   AMSHIM_HTTP_DUMP=C:/some/dir saves HTTP response bodies (a sandboxed prefix has no Z: drive)
# The service must not be running (bin/amctl quit).
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$ROOT/bin/amd-env.sh"
[ -n "$1" ] || { sed -n '4,6p' "$(readlink -f "$0")"; exit 2; }
if am_busy; then echo "the service or the agent is running; stop it first: bin/amctl quit" >&2; exit 1; fi
am_need_app
am_logs
cd "$APPDIR" || exit 1
WINEDEBUG=${3:-err,fixme,warn+seh,+amshim,+ampkg} timeout "${2:-60}" wine AMPLibraryAgent.exe -Embedding > "$LOGS/$1.log" 2>&1
echo "exit=$? (124 = still running at timeout, i.e. good)"
wineserver -k 2>/dev/null
