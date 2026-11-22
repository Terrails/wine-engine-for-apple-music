# SPDX-License-Identifier: LGPL-2.1-or-later
# Sourced by the scripts (not run): default paths, the Wine prefix and the Wine session.
# The caller sets ROOT to the repository first, resolved through symlinks, e.g.
#   ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"; . "$ROOT/bin/amd-env.sh"
# so bin/ can be linked into ~/.local/bin.
#
# Defaults (override any of them in the environment):
#   AM_PREFIX     ${XDG_DATA_HOME:-~/.local/share}/wine-engine-for-apple-music/prefix
#   AM_LOG_DIR    ${XDG_STATE_HOME:-~/.local/state}/wine-engine-for-apple-music/logs
#   AM_CACHE_DIR  ${XDG_CACHE_HOME:-~/.cache}/wine-engine-for-apple-music
#                 (metadata/ from NuGet, msix/<version>/ unpacked package, applerootca/)
#   AM_METADATA_DIR  $AM_CACHE_DIR/metadata (Windows SDK + WinUI metadata for the shim build)
#   AM_PYTHON     python3 (needs dnfile: pip install -r requirements.txt)
# Relative paths are taken against the directory the script was started from.
# WINEPREFIX from the environment is ignored on purpose: these scripts never touch your default
# Wine prefix, only AM_PREFIX.
AM_NAME=wine-engine-for-apple-music
AM_PREFIX="${AM_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/$AM_NAME/prefix}"
AM_LOG_DIR="${AM_LOG_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/$AM_NAME/logs}"
AM_CACHE_DIR="${AM_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/$AM_NAME}"
AM_METADATA_DIR="${AM_METADATA_DIR:-$AM_CACHE_DIR/metadata}"
AM_PYTHON="${AM_PYTHON:-python3}"
# absolute, once, here: the scripts cd elsewhere (shim/build.sh, make -C, the app directory) and
# Wine needs an absolute prefix path
case $AM_PREFIX in /*) ;; *) AM_PREFIX="$PWD/$AM_PREFIX" ;; esac
case $AM_LOG_DIR in /*) ;; *) AM_LOG_DIR="$PWD/$AM_LOG_DIR" ;; esac
case $AM_CACHE_DIR in /*) ;; *) AM_CACHE_DIR="$PWD/$AM_CACHE_DIR" ;; esac
case $AM_METADATA_DIR in /*) ;; *) AM_METADATA_DIR="$PWD/$AM_METADATA_DIR" ;; esac
export WINEPREFIX="$AM_PREFIX"
# A WINEARCH from your profile (e.g. win32) would make wineboot create a prefix the x64 app cannot
# use; without it a new prefix gets Wine's default, 64-bit. Other Wine variables (WINE, WINESERVER,
# WINEDLLOVERRIDES, ...) are passed through as they are.
unset WINEARCH
WRAPPER="$ROOT/wrapper"
APPDIR="$WINEPREFIX/drive_c/Program Files/AppleMusic"
LOGS="$AM_LOG_DIR"

# the log directory (created on first use)
am_logs() { mkdir -p "$LOGS"; }

# AM_PYTHON must have dnfile (and pefile, which dnfile pulls in)
am_python() {
    if ! "$AM_PYTHON" -I -c 'import dnfile, pefile' 2>/dev/null; then
        echo "error: $AM_PYTHON cannot import dnfile/pefile." >&2
        echo "  pip install -r $ROOT/requirements.txt   (in a venv; then set AM_PYTHON=<venv>/bin/python)" >&2
        exit 1
    fi
}

# build wrapper/build/amd.exe + libampulse.so (default) or another make target, only what changed
am_build() { make -s -C "$WRAPPER" "${1:-all}" >&2; }

# copy a built program next to the app (only when it changed)
am_install() { cmp -s "$WRAPPER/build/$1" "$APPDIR/$1" || cp -f "$WRAPPER/build/$1" "$APPDIR/$1"; }

# is a wrapper program (amd.exe / lab.exe) running in some prefix?
am_running() { pgrep -f "^$1( |\$)" >/dev/null 2>&1; }

# is the service or the app's agent running (in some prefix; see KNOWN_ISSUES.md)? The scripts
# that change the prefix or end its Wine session refuse to start then.
am_busy() { am_running amd.exe || pgrep -f 'AMPLibraryAgen[t]\.exe' >/dev/null 2>&1; }

# the app must be installed in the prefix (setup-prefix.sh)
am_need_app() {
    [ -f "$APPDIR/AMPLibraryAgent.exe" ] && return 0
    echo "error: no app in $APPDIR; run setup-prefix.sh first (AM_PREFIX=$AM_PREFIX)" >&2
    exit 1
}

# Start from a clean Wine session. If the agent was killed or crashed, Wine's rpcss keeps its
# stale COM class registration and the next start gets a dead object (IAMPLibrary creation fails
# with 0x800706ba, nothing plays). This stops everything running in the prefix, which is meant to
# be dedicated to this app. AM_KEEP_WINESERVER set to any value (even 0) skips it.
am_clean_session() {
    [ -n "$AM_KEEP_WINESERVER" ] && return 0
    for _ in 1 2 3 4 5 6; do am_running amd.exe || break; sleep 0.5; done   # a daemon that is quitting
    if wineserver -k 2>/dev/null; then
        for _ in $(seq 20); do wineserver -k 2>/dev/null || break; sleep 0.2; done
    fi
}

# keep the previous log as <name>.prev.log
am_rotate_log() { am_logs; [ -f "$LOGS/$1.log" ] && mv -f "$LOGS/$1.log" "$LOGS/$1.prev.log"; true; }

# the API token written by the service
am_token() { cat "$WINEPREFIX"/drive_c/users/*/AppData/Local/AppleMusicWrapper/token 2>/dev/null; }
