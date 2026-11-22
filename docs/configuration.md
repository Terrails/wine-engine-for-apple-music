# Configuration

Everything is configured through environment variables; there is no configuration file. Set them
for one command (`AM_PORT=7700 bin/amd`) or export them in your shell profile.

## Paths

The defaults follow the XDG base directory specification:

| What | Default | Variable |
|---|---|---|
| Wine prefix | `${XDG_DATA_HOME:-~/.local/share}/wine-engine-for-apple-music/prefix` | `AM_PREFIX` |
| Logs | `${XDG_STATE_HOME:-~/.local/state}/wine-engine-for-apple-music/logs` | `AM_LOG_DIR` |
| Cache: `metadata/` (NuGet), `msix/` (unpacked package), `applerootca/` | `${XDG_CACHE_HOME:-~/.cache}/wine-engine-for-apple-music` | `AM_CACHE_DIR` |
| WinRT metadata for the shim build | `$AM_CACHE_DIR/metadata` | `AM_METADATA_DIR` |

`WINEPREFIX` is never used: the scripts set it themselves from `AM_PREFIX`, so this project
never touches your default Wine prefix, whatever your profile sets. `WINEARCH` is unset too (a
`win32` from your profile would create a prefix the x64 app cannot use); other Wine variables
such as `WINEDLLOVERRIDES` are passed through. Relative paths in these variables are taken
against the directory the command was started from.

Inside the prefix (Windows paths; `%LOCALAPPDATA%` is
`drive_c/users/<you>/AppData/Local`):

| Path | What |
|---|---|
| `C:\Program Files\AppleMusic` | the app, `amd.exe`, `ampkg.dll` |
| `%LOCALAPPDATA%\AppleMusicWrapper\token` | the API token, new on every start |
| `%LOCALAPPDATA%\AppleMusicWrapper\media-name` | the current track's name for the mixer |
| `HKCU\Software\AppleMusicWrapper\Settings` | the app's `ApplicationData` settings (shim) |

## Environment variables

All variables in one table. "Read by" names the script or component that reads the variable;
`amd.exe` is the wrapper service, `ampkg.dll`/`amshim.dll` are shims loaded into the wrapper
and/or the agent (`AMPLibraryAgent.exe`, which inherits the wrapper's environment). Variables
marked "dev" are for experiments and debugging ([development.md](development.md),
[troubleshooting.md](troubleshooting.md)). Switches described as "set to any value" are checked
only for being set: `0` turns them **on** as well; unset them to turn them off.

| Variable | Default | Read by | Effect | Privacy |
|---|---|---|---|---|
| `AM_PREFIX` | `$XDG_DATA_HOME/wine-engine-for-apple-music/prefix` | every script (`bin/*`, `setup-prefix.sh`, `shim/install.sh`, `dev/*`) | the Wine prefix to set up and run in | the prefix holds your sign-in session and library |
| `AM_LOG_DIR` | `$XDG_STATE_HOME/wine-engine-for-apple-music/logs` | `bin/amd`, `bin/amd-signin`, `dev/*` | where `amd.log`, `amd.prev.log`, `signin.log` and dev logs go | logs contain Apple request lines with cookies; do not share unfiltered |
| `AM_CACHE_DIR` | `$XDG_CACHE_HOME/wine-engine-for-apple-music` | `setup-prefix.sh`, `tools/fetch-metadata.sh`, `shim/build.sh`, `tools/q`, `tools/gen.py`, `make lab` (`dev/lab/iids.sh`) | NuGet metadata, the unpacked MSIX, the Apple Root CA download | none |
| `AM_METADATA_DIR` | `$AM_CACHE_DIR/metadata` | `tools/fetch-metadata.sh`, `shim/build.sh`, `tools/gen.py`, `tools/q` | where the Windows SDK and Windows App SDK metadata (`sdk/`, `winui/`) is downloaded to and read from | none |
| `AM_PYTHON` | `python3` | `setup-prefix.sh`, `shim/build.sh`, `tools/q`, `make lab` | Python interpreter with `dnfile` and `pefile` installed (`requirements.txt`) | none |
| `AM_ALLOW_UNTESTED_MSIX` | unset | `setup-prefix.sh` | `1`: accept an Apple Music package whose version or SHA-256 differs from the tested one (with a warning) instead of refusing it | none |
| `AM_PORT` | `7654` | `bin/amd` (when no port argument is given), `bin/amctl`, `dev/gapmon.py`, the Sidra fork (passes it to `bin/amd` as its argument) | TCP port of the API on `127.0.0.1`; `bin/amd <port>` overrides it for the service. Set the same value for the service and its clients | the API needs the token; only local processes can reach it |
| `AM_STOREFRONT` | `us` | `amd.exe` (`/play` without `storefront=`), UIs, `test/mock-daemon.mjs` | default storefront (country code) for album links | none |
| `AM_KEEP_WINESERVER` | unset | `bin/amd`, `bin/amd-signin`, `dev/lab.sh` (`bin/amd-env.sh`) | set to any value (even `0`): do not end the prefix's Wine session before starting, and in `bin/amd` after `/quit`. `bin/amd-signin` and `dev/*.sh` still end it when they finish. Without it, a stale session (a crashed agent's COM registration) can leave the engine unusable | none |
| `AMD_WINEDEBUG` | `err` | `bin/amd` | the `WINEDEBUG` channels for the service, e.g. `err,+mmdevapi` | traces can contain request data; `+amcall`/`+amshim` log shim calls with arguments |
| `WINEDEBUG` | ignored | `bin/amd` uses `AMD_WINEDEBUG`; `bin/amd-signin` always runs with `err`; `setup-prefix.sh` and `shim/install.sh` with `-all`; `dev/lab.sh` and `dev/run-agent.sh` take it as an argument | not passed through, so a `WINEDEBUG` from your profile cannot make the sign-in log record more | sign-in stays at `err` on purpose |
| `PULSE_LATENCY_MSEC` | `100` (set by `bin/amd` unless already set) | libpulse inside the Wine session | buffer target on the PulseAudio side. Smaller values (without it the PulseAudio target is about 25 ms) produce clicks under CPU load and pull the whole PipeWire graph down to 256-frame cycles; 100 ms costs ~70 ms of output delay | none |
| `AM_AUDIO_NAME` | `Apple Music` | `bin/amd` (as `PULSE_PROP_OVERRIDE_application.name`), `dev/gapmon.py` (to find the stream) | client name of the audio stream in mixers | none |
| `AM_AUDIO_TRACK_NAME` | `1` | `bin/amd` | `0`: do not preload `libampulse.so`, the stream keeps Wine's own name (`audio stream #n`) instead of `<artist> - <title>` | with `1`, the current track is visible to every local program that lists audio streams |
| `AM_MEDIA_NAME_FILE` | set by `bin/amd` | `libampulse.so` (Linux side) | internal: the file `amd.exe` writes the track name to | none |
| `AM_ACCEPT_WELCOME` | set to `1` by `bin/amd`, `bin/amd-signin`, `dev/lab.sh` | `amd.exe`, `lab.exe` | presses the accept button of the app's first-run privacy notice on your behalf; without it the app does not start its services. See [install.md](install.md#sign-in) | running these scripts accepts Apple's notice |
| `AM_TS` | unset | `amd.exe` | set to any value (even `0`): milliseconds since start on every `wrapper:` log line (start-up timing) | none |
| `AM_AGENT_OSLOG` | unset | `ampkg.dll` (agent) | set to any value (even `0`): the agent's own log (cloud library, database, playback) as `applog[n]:` lines in `amd.log`; format strings only | without `AM_OSLOG_ARGS` no values; with it, account identifiers |
| `AM_OSLOG_ARGS` | unset; **removed by `bin/amd` and `bin/amd-signin`** | `amd.exe` (`oslog.c`), `ampkg.dll` | set to any value (**even `0`**): expand all arguments of the app's log messages (normally private ones are masked). Only takes effect when you run `amd.exe` yourself | **do not use with your real account** unless you filter the output: account data, tokens, request details |
| `CFNETWORK_IO_LOG`, `CFNETWORK_IO_LOG_STDERR` | unset | the app's CFNetwork, in every app process | `1`: dump socket I/O (`_STDERR`: to the log) | **everything** sent and received, including credentials and cookies; test accounts only |
| `AMSHIM_HTTP_DUMP` | unset | `amshim.dll` | `C:/some/dir` (a path inside the prefix): save every HTTP response body as `NNN.bin` | bodies contain account and library data; delete the directory afterwards |
| `AM_TEST_USER`, `AM_TEST_PASS` | unset; **removed by `bin/amd` and `bin/amd-signin`** | `amd.exe signin` | dev: submit these credentials instead of showing the prompt (for testing sign-in failures with a made-up account) | the password sits in the environment and shell history; never use real credentials |
| `AM_DIALOG_AUTO` | unset; removed by `bin/amd` and `bin/amd-signin` | `amd.exe` (`prompt.c`) | dev: presses a native prompt's last button after 1.5 s (unattended tests) | none |
| `AM_AUDIO_IDLE_STREAM` | unset | `ampkg.dll` (agent) | set to any value (even `0`): let the agent keep its permanent silent start-up audio stream (by default the shim answers it without a real stream). Useful to check routing before anything plays | none |
| `AM_HANDLE_DEBUG` | unset | `ampkg.dll` | dev: set to any value (even `0`): reports `CloseHandle` calls that fail (double closes) | none |
| `AM_EXPLORE` | unset | `lab.exe` (`dev/lab.sh`) | dev: list the current view model and which known interfaces it answers to (needs `make lab` with the generated IID table) | none |
| `AM_SEARCH` | unset | `lab.exe` | dev: run the app's sidebar search with this query and list the page | none |
| `AM_PLAY_TITLE` | `SquareLockup` | `lab.exe` | dev: with `AM_SEARCH`, which item to pick (title or class part) | none |
| `AM_PLAY` | unset | `lab.exe` | dev: with `AM_SEARCH`, play the picked item for 35 s (**makes sound**) | none |
| `AM_DEEPLINK` | unset | `lab.exe` | dev: open a deep link (`musics://music.apple.com/us/album/<id>`) and list the page | none |
| `AM_DUMP` | unset | `lab.exe` | dev: with `AM_DEEPLINK`, every item of the page with its play action's class | none |
| `AM_PLAY_ITEM` | unset | `lab.exe` | dev: with `AM_DEEPLINK`, `s:i` runs the play action of item `i` in section `s` (**makes sound**) | none |
| `AM_QUEUE` | unset | `lab.exe` | dev: with `AM_PLAY_ITEM`, dump the play queue | none |
| `AM_JUMP` | unset | `lab.exe` | dev: with `AM_QUEUE`, jump to the queue item with this catalogue ID | none |
| `AM_NOWPLAYING` | unset | `lab.exe` | dev: with `AM_PLAY_ITEM`, every field of the now-playing item | none |
| `AM_CD_TEST` | unset | `lab.exe` | dev: round-trip `ContentDescriptor` JSON candidates (`a`, `b`, ...) | none |
| `AM_CD_DUMP` | unset | `lab.exe` | dev: with `AM_DEEPLINK`/`AM_SEARCH`, serialize the first catalogue item's descriptor | none |
| `AM_CLOUD`, `AM_CLOUD_ON` | unset | `lab.exe` | dev: report the cloud library state; `AM_CLOUD_ON=1` also turns syncing on | library state of your account in the log |
| `AM_SILENT` | unset | `lab.exe` | dev: engine volume 0 for the run | none |
| `AM_RESTORE_VOLUME` | `1` | `lab.exe` | dev: with `AM_SILENT`, the volume set at the end | none |
| `AM_WAIT` | `8000` | `lab.exe` | dev: milliseconds to let the app settle before the experiment | none |
| `AM_POLL` | `5` | `lab.exe` | dev: seconds of life-cycle/agent-state reports at the end; above 5 also makes a read-only agent call each second | none |
| `WINE_INCLUDE_DIR` | searched | `shim/ps/build.sh` | the directory holding Wine's IDL files (`inspectable.idl`) for `widl`, when the search next to `widl` and in the usual package locations does not find it | none |
| `AM_SHIMS_BUILT` | unset | `shim/install.sh`, `shim/build.sh`, `shim/ps/build.sh` | internal: set to `1` by `setup-prefix.sh`, which has just built the shims, so `shim/install.sh` only installs them | none |
| `WINMD_DIR` | unset | `tools/q`, `make lab` (`dev/lab/iids.sh`) | dev: metadata directory to read instead of the default (`tools/q`: the SDK metadata; `make lab`: the installed app, then `$AM_CACHE_DIR/msix/<version>`), e.g. the app's own `.winmd` files ([development.md](development.md)) | none |
| `MOCK_SPEED`, `MOCK_AUTOPLAY` | `1`, unset | `test/mock-daemon.mjs` | dev: clock multiplier of the mock service; `MOCK_AUTOPLAY=1`: an unrelated track follows the end of the last one, like the engine's autoplay | none |
| `AM_AGENT_DIR`, `AM_WEBUI_*` | | the Sidra fork | where the fork finds this checkout, and the fork's own settings: see `README-agent.md` in the fork's `wine-engine` branch | see there |

Other variables that are useful here:

- `PULSE_SINK=<sink>` (libpulse) for `bin/amd`: route the engine to a specific PulseAudio sink,
  e.g. a null sink for silent tests ([troubleshooting.md](troubleshooting.md)). The agent
  inherits it.
- `DBG=<WINEDEBUG>` and `T=<seconds>` for `dev/dbg-agent.sh` (defaults `err` and 90).
- `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`: the base directories of the default paths
  above.

## Logging levels that are not environment variables

Two of the app's components have their own log switches. Both log more than the default and are
**not for logs you intend to share** above `error`:

- **AuthKit (sign-in)**: registry value `HKCU\Software\Apple Inc.\Internet Services\Logging`,
  named after the program (`amd`), `none|error|warn|info|debug|verbose`. `amd.exe` sets `error`
  when the value does not exist. `info` can and `debug` does log your Apple Account name.
- **Push service** (`ApplePushDirect.dll`): the CFPreferences key `ShouldLog` of the agent, a
  **string** (`error`, `warning`, `debug`). At `debug` the log contains push tokens and user
  identifiers. Never set it as a boolean or number: the DLL expects a string and the agent
  crashes.

How to set them: [troubleshooting.md](troubleshooting.md).
