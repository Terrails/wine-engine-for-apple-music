#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run an experiment (dev/lab/lab.c; what it does is chosen by environment variables, see the top of
# that file). Example: AM_EXPLORE=1 dev/lab.sh 60
# usage: dev/lab.sh [seconds=90] [WINEDEBUG=err,fixme,+ampkg,+amshim]     log: $AM_LOG_DIR/lab.log
# The service must not be running (bin/amctl quit). lab.exe keeps its own CFPreferences
# (domain lab.exe), but shares the app's ApplicationData settings with amd.exe (registry, shim/).
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$ROOT/bin/amd-env.sh"
set -e
if am_busy; then echo "the service or the agent is running; stop it first: bin/amctl quit" >&2; exit 1; fi
am_need_app
am_logs
am_build lab
am_clean_session
am_install lab.exe
cd "$APPDIR"
set +e
# accept the first-run notice (only shown until it has been accepted once): the app does not
# navigate while it is open
AM_ACCEPT_WELCOME=1 WINEDEBUG="${2:-err,fixme,+ampkg,+amshim}" timeout "${1:-90}" wine lab.exe > "$LOGS/lab.log" 2>&1
echo "exit=$?"
wineserver -k 2>/dev/null
exit 0
