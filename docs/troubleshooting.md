# Troubleshooting

Find your symptom below. Each section says what to look at and what the usual causes are. Known
open problems are listed in [KNOWN_ISSUES.md](../KNOWN_ISSUES.md); how the parts fit together is
in [architecture.md](architecture.md).

The commands on this page assume `AM_PREFIX` and `AM_LOG_DIR` are set. With the defaults
([configuration.md](configuration.md)):

```sh
: "${AM_PREFIX:=${XDG_DATA_HOME:-$HOME/.local/share}/wine-engine-for-apple-music/prefix}"
: "${AM_LOG_DIR:=${XDG_STATE_HOME:-$HOME/.local/state}/wine-engine-for-apple-music/logs}"
APPDIR="$AM_PREFIX/drive_c/Program Files/AppleMusic"
PROFILE="$AM_PREFIX/drive_c/users/$USER"
```

Wine commands on this page run in the project's prefix only: always with
`WINEPREFIX="$AM_PREFIX"`, and with the service stopped (`bin/amctl quit`) unless a section says
otherwise.

## Logs and privacy

| Log | Where | What |
|---|---|---|
| service | `$AM_LOG_DIR/amd.log` (previous run: `amd.prev.log`) | everything `bin/amd` runs: `wrapper:` lines from `amd.exe`, `oslog[n]:` and `asl[n]:` lines from the app, `applog[n]:` lines from the agent (optional), Wine's own `err:`/`fixme:` lines |
| sign-in | `$AM_LOG_DIR/signin.log` | the same for `bin/amd-signin` |
| AuthKit | `$PROFILE/AppData/Local/Packages/AppleInc.AppleMusicWin_nzyj5cx40ttqa/LocalCache/Local/Logs/AuthKitWin.<date>_<time>.log` | AuthKit's own log; created only when something is logged |
| app's system log | `$PROFILE/AppData/Roaming/Apple Computer/Logs/asl.*.log` | ASL messages of the agent (e.g. the push service) |
| experiments | `$AM_LOG_DIR/lab.log`, `$AM_LOG_DIR/<name>.log` | `dev/lab.sh`, `dev/run-agent.sh` ([development.md](development.md)) |

**Privacy.** Read this before you share a log.

- `amd.log` and `signin.log` contain the app's request lines, including cookies. Remove the
  lines containing `Submitting the HTTP request` and read the rest before sharing. With the
  default settings the wrapper masks anything in the app's log arguments that looks like an
  e-mail address or a long number, and writes no arguments of ordinary messages at all.
- The defaults never record what you type: `bin/amd-signin` runs with Wine tracing at `err`,
  and both `bin/amd` and `bin/amd-signin` remove `AM_OSLOG_ARGS`, `AM_TEST_USER`,
  `AM_TEST_PASS` and `AM_DIALOG_AUTO` from the environment.
- Each switch below that logs more says what it exposes. As a rule, **do not turn on
  `AM_OSLOG_ARGS`, `CFNETWORK_IO_LOG*`, AuthKit above `error` or the push log at `debug` with
  your real account** unless you filter the output yourself.

## The service does not start or the API does not answer

`bin/amd` runs in the foreground; the API is ready when `amd.log` shows

```
wrapper: SERVE ready on http://127.0.0.1:7654 (token in C:\users\…\AppleMusicWrapper\token)
```

Cold start takes a few seconds plus Wine's own start. If that line does not come:

- **`SERVE cannot start the server on port …`**: the port is in use, usually by a service that is
  still running. `bin/amctl status` answers if it is; `bin/amctl quit` stops it. Or use another
  port (`AM_PORT`).
- **Exit code 4, `SERVE no working agent`** or **`playback service started=0`**: the agent did
  not come up within 30 s. Look further up for `create_instance(` … `IAMPLibrary` … `failed`
  or `0x800706ba`:
  - **Stale COM registration after a crash.** If the agent was killed or crashed, Wine's rpcss
    keeps its class registration and the next start gets a dead object. `bin/amd` normally
    ends the prefix's Wine session first; if you run with `AM_KEEP_WINESERVER=1`, end it
    yourself: `bin/amctl quit`, then `WINEPREFIX="$AM_PREFIX" wineserver -k`, then start again.
  - The prefix is incomplete: run `setup-prefix.sh` again (it is safe to repeat and keeps your
    sign-in and library).
- **`welcome dialog pending: yes`** and nothing after it: the first-run notice is waiting.
  `bin/amd` accepts it (`AM_ACCEPT_WELCOME=1`); if you started `amd.exe` some other way, set that
  variable.
- **`Failed to find library for <class>`** or crashes right after `AMP.Services` loads: the
  shims are missing or stale. Stop the service and run `shim/install.sh` (it refuses while the
  service or the agent runs).
- **`the service is running; stop it first`** from `bin/amd-signin` or `dev/lab.sh`: only one
  program may drive the engine; `bin/amctl quit` first.
- **401 from every request**: your client holds the token of an earlier start. Re-read the token
  file ([api.md](api.md)).
- **Exit code 4 later on** (`SERVE the agent is gone`): the agent died. Start the service again;
  the log before that line shows why (see "Nothing plays").

## Sign-in fails

`bin/amd-signin` shows native prompts for the account and the two-factor code, and prints
`exit=0 (0 = signed in)` on success. The sign-in runs inside the service process, through
AuthKit and CFNetwork ([architecture.md](architecture.md), "Sign-in and AuthKit").

The alert text identifies the error (`AKAuthenticationError` codes):

- **-7005, "There was an error connecting to the Apple Account server."** The HTTPS request to
  Apple failed at the transport level: network, proxy or TLS. A wrong password gives a
  different message. Check:
  - The Apple Root CA is in the prefix's Root store (`setup-prefix.sh` adds it; without it the
    connection to `gsa.apple.com` is dropped before anything is sent). Re-run
    `setup-prefix.sh` if in doubt.
  - No proxy is configured in the prefix's Internet Settings.
  - Then read the AuthKit log (below) for the `didFail Request did fail: …` line before
    retrying.
- **-7006**: wrong account name or password.
- **-7003**: the two-factor prompt was cancelled.
- `AuthKit wants server-provided web UI (unsupported)` in the log: Apple asked for a web page
  (for example an account repair flow). That needs a WebView and is not supported in this
  setup.
- `ADIOTPRequest failed with error -45061` before the first successful sign-in is the machine
  not being provisioned yet. Harmless.

**AuthKit log.** AuthKitWin writes its own log (not through the app's log) when the registry
value `HKCU\Software\Apple Inc.\Internet Services\Logging`, named after the executable without
`.exe` (`amd`), is set to `none`, `error`, `warn`, `info`, `debug` or `verbose`. `amd.exe` sets
`amd=error` at start unless the value exists. The file appears only when something is logged
(path in "Logs and privacy").

- `error` holds the HTTP failure and the `LoginFSM` result, without account data.
- `info` **can** and `debug` **does** log the Apple Account name. Raise it only with a test
  account, and set it back afterwards:

```sh
WINEPREFIX="$AM_PREFIX" wine reg add 'HKCU\Software\Apple Inc.\Internet Services\Logging' /v amd /d info /f
WINEPREFIX="$AM_PREFIX" wine reg add 'HKCU\Software\Apple Inc.\Internet Services\Logging' /v amd /d error /f
```

**Network trace.** `CFNETWORK_IO_LOG=1 CFNETWORK_IO_LOG_STDERR=1` dumps the app's socket I/O
into the log: a record type of only `0x16` (handshake) means the connection was dropped during
TLS, `0x17` means data flowed. This logs **everything** sent and received, credentials
included; use it only with a made-up account. `AM_TEST_USER`/`AM_TEST_PASS` submit such an
account without the prompt (development only; `bin/amd-signin` removes them, so run
`amd.exe signin` yourself for that).

## Nothing plays, or the engine stops responding

Check `bin/amctl status` first:

- `"signedIn": false`: sign in (`bin/amd-signin`, with the service stopped).
- `"ready": false` or 503 `player not ready`: the engine is still starting; wait a few seconds.
- `"agentHung": true`: an agent call has not returned within 5 s; `agentPhase` names it. The log
  has `oslog[3]: **ERROR**: Agent reported as hung (<call>)`, and
  `Agent resumed after being reported as hung` when it comes back. If it does not recover,
  restart the service (`bin/amctl quit`, `bin/amd`).
- `/play` returns **404 `item not found on the album page`**: the album page did not contain the
  song within about 10 s. Check the IDs (song and album must belong together) and the
  storefront: an album that is not available in your storefront has no page there. The service
  defaults to `us` (`AM_STOREFRONT`, or `storefront=` per request).
- `/play` returns 200 but `nowPlaying` stays `null` or shows another track: watch the log for
  `play: …` lines (which way playback was started and how long it took) and for errors from
  the agent after them.
- The service exits with code 4: the agent died. The lines before
  `SERVE the agent is gone` show its last messages; a backtrace needs the agent's own log or
  `dev/dbg-agent.sh` ([development.md](development.md)).

To test a UI's "not responding" state against the real engine, pause the agent with
`kill -STOP` and make an agent call (for example `bin/amctl volume 0.5`); `kill -CONT` resumes
it. Find the process with `ps -eo pid=,comm= | grep AMPLibraryAgen` (not `pgrep -f`, see
[development.md](development.md), "Lessons"). `AM_POLL=<n> dev/lab.sh` prints the life-cycle
and agent state each second. Against the mock, `POST /mock/hang` does the same
([api.md](api.md)).

## Crackling, clicks or dropouts

Two causes have been found; check both.

1. **Registry saves block Wine (dropouts of a fraction of a second, at intervals of about 30 s,
   longer under load).** Wine's Bluetooth driver records every Bluetooth device the host sees in
   the prefix's registry, and wineserver blocks every Wine thread while it rewrites a large
   registry. `setup-prefix.sh` disables `winebth.sys` and deletes the recorded devices. Check:

   ```sh
   ls -lh "$AM_PREFIX/system.reg"                     # a few MB is normal; tens of MB is not
   grep -c 'Enum\\\\WINEBTH' "$AM_PREFIX/system.reg"  # should be 0
   grep '"winebth.sys"=""' "$AM_PREFIX/user.reg"      # the disabled DLL override
   ```

   If devices are there, stop the service and run `setup-prefix.sh` again.
2. **The pulse-side buffer is too small (clicks under CPU load).** `bin/amd` sets
   `PULSE_LATENCY_MSEC=100`. If you set it yourself to a smaller value, raise it again; if
   clicks remain under heavy load, try a larger one. Background:
   [architecture.md](architecture.md), "Design notes".

To measure instead of listening, `dev/gapmon.py` follows the engine's stream and the default
sink's monitor and logs silent gaps inside music with the track position
([development.md](development.md)). Attaching any monitor to the stream causes one click itself
(the graph changes).

## The mixer shows the wrong name

The client is named after `AM_AUDIO_NAME`, the stream after the track (`<artist> - <title>`)
through the preloaded `libampulse.so` ([architecture.md](architecture.md), "Audio"). If the
stream keeps Wine's name (`audio stream #n`): check that `AM_AUDIO_TRACK_NAME` is not `0`, that
`wrapper/build/libampulse.so` was built (it needs the libpulse headers), and that
`$PROFILE/AppData/Local/AppleMusicWrapper/media-name` changes while a track plays. Some mixers
hide stream names they consider generic (KDE Plasma: names matching
`playback|audio|stream|alsa|pulse|pipewire`), so a title containing such a word shows as the
client name only.

## Push service log

The push service (`ApplePushDirect.dll`, notifications about library changes made elsewhere)
logs through ASL into `$PROFILE/AppData/Roaming/Apple Computer/Logs/asl.*.log` when the
CFPreferences key `ShouldLog` in the process's domain (the executable name,
`AMPLibraryAgent.exe`) is a **string**: `none`/`no`/`false`/`0` (off), `error`, `warning`,
`debug`/`yes`/`true`/`1` (case-insensitive; read once per process start). Set it with the app's
own `defaults.exe` while the service is stopped:

```sh
cd "$APPDIR"
WINEPREFIX="$AM_PREFIX" wine defaults.exe write AMPLibraryAgent.exe ShouldLog -string debug
WINEPREFIX="$AM_PREFIX" wine defaults.exe delete AMPLibraryAgent.exe ShouldLog
```

**Never use `-bool` or `-int`**: the DLL passes the value straight to `CFStringCompare` and the
agent crashes at start (access violation in `objc.dll`). At `debug` the log contains push
tokens and user identifiers; do not share it. The key turns on only the push service's log, not
the agent's general logging.

Healthy push start-up: `Received connected response OK`, a public token, topics filtered, then
keep-alives.

## Agent log

The agent's own log (cloud library, database, playback) goes into `amd.log` as `applog[n]:`
lines with

```sh
AM_AGENT_OSLOG=1 bin/amd
```

These are format strings only: `bin/amd` removes `AM_OSLOG_ARGS`, so no values are expanded.
With values (`AM_OSLOG_ARGS=1`, which means running `amd.exe serve` from `$APPDIR` yourself
with `WINEPREFIX="$AM_PREFIX"`), the log holds account identifiers; mask them before sharing.

Useful lines: `db> Loaded: tracks = …` and `--- TrackData Totals ---` (library database),
`Cloud Library> Startup:` and `iCloud Library Client> … in sync with the server` (cloud
library).

The wrapper's own diagnostics are always on: `wrapper/src/cxxdbg.c` prints every C++ exception
the app throws (type, HRESULT or `what()`, a module+offset backtrace) and a backtrace for
crashes and breakpoints (`FATAL exception …`), and `oslog.c` shows the service's app log, with
the expression of failed assertions (`Assertion failure: (<expr>)`). For timing questions,
`AM_TS=1 bin/amd` appends `@<n>ms` (milliseconds since the first line) to every `wrapper:` line.

## Silent tests on the real engine

To test playback without sound, route the engine to a null sink. The agent inherits the
service's environment, so `PULSE_SINK` reaches it:

```sh
MOD=$(pactl load-module module-null-sink sink_name=am_silent)
bin/amctl quit
PULSE_SINK=am_silent AM_AUDIO_IDLE_STREAM=1 bin/amd &
pactl list sink-inputs          # the engine's stream must be on am_silent before anything plays
# ... tests ...
bin/amctl quit
pactl unload-module "$MOD"
```

`AM_AUDIO_IDLE_STREAM=1` brings back the agent's silent start-up stream, so you can verify the
routing before anything audible plays. **Volume 0 is not a safe substitute**: with nothing
playing, the engine's own volume reading can be stale, and the next track may play at a different level.

## Resetting the app's settings

The app's `ApplicationData` settings (first-run notice, sidebar state, ...) live in the
prefix's registry ([shims.md](shims.md), "Settings"). With the service stopped:

```sh
WINEPREFIX="$AM_PREFIX" wine reg delete 'HKCU\Software\AppleMusicWrapper\Settings' /f
```

The next start shows (and `bin/amd` accepts) the first-run notice again. The streaming quality
is not among these settings: the agent keeps it in its own preferences
(`$PROFILE/AppData/Roaming/Apple Computer/Preferences/AMPLibraryAgent.exe.plist`); set it with
`POST /quality`. To start over completely, delete the prefix and run `setup-prefix.sh` again
([install.md](install.md)); you will have to sign in again.

## Finding what is missing (debugging Wine gaps)

When something new fails, grep the log for:

| Pattern | Meaning |
|---|---|
| `Failed to find library for` | a WinRT class nobody implements (candidate for `shim/classes.txt`) |
| `fixme:amshim:.* stub` | a generated amshim method that is now being called (candidate for an `OV_*` override) |
| `RoOriginateLanguageException`, `amlog[3]` | errors reported by the app's WIL error handling |
| `no interface`, `unset object property` | amshim objects asked for something they do not have |
| `FATAL exception` | crash or breakpoint, with backtrace |

Tracing: `AMD_WINEDEBUG=err,+amcall bin/amd` traces overridden amshim methods with their
results, `+amshim` the generated ones, `+ampkg` the package shim, `+mmdevapi` Wine's audio
clients. Traces can contain request data; treat them like the log.

How to add the missing piece: [development.md](development.md).
