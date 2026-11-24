# wine-engine-for-apple-music

Runs the playback and library engine of the **Apple Music for Windows** app under
[Wine](https://www.winehq.org/) on Linux and puts a small local HTTP API in front of it, so a
user interface of your choice can sign in, play the Apple Music catalogue (AAC and lossless) and
control playback.

> Unofficial. Not affiliated with, endorsed by or sponsored by Apple Inc. Apple Music is a
> trademark of Apple Inc.

## What this is, and what it is not

- It is a **wrapper service** (`amd.exe`, a Windows program built with `winegcc`) that loads the
  app's own services layer (`AMP.Services.dll`) without its user interface, brings it up the way
  the app does, and talks to the app's background process `AMPLibraryAgent.exe` (library,
  playback, FairPlay, sign-in). On top of that it serves an HTTP + JSON API on `127.0.0.1`.
- It is a set of **compatibility shims** for what Wine does not provide yet (WinRT classes,
  package identity, a crypt32 encoder, proxy/stubs, two small stubs), plus a script that sets up
  a dedicated Wine prefix with the app in it.
- It is **not** a copy of Apple Music. Nothing from the app is in this repository. You need
  **your own copy of the app** (its MSIX package, which you obtain yourself; see
  [docs/install.md](docs/install.md)) and an **Apple Music subscription** on your Apple Account.
- It is **not** a full desktop client. The app's own front end (WinUI 3) cannot run under Wine:
  it needs in-process ALPC and a working DirectComposition, which Wine does not have. So **you
  bring a UI**:
  - the [`wine-engine` branch](https://github.com/Terrails/sidra/tree/wine-engine) of a fork of
    [Sidra](https://github.com/wimpysworld/sidra): music.apple.com in a desktop window, with
    playback going through this engine (see `README-agent.md` in that branch), or
  - your own UI or script over the HTTP API ([docs/api.md](docs/api.md)); `bin/amctl` is a
    minimal command-line client.

## Status

Version 0.1.0 (unreleased). Works with Apple Music for Windows **1.1792.39941.0** (x64).

Working:

- Sign-in to an Apple Account with two-factor authentication, in native prompts; the session
  persists across restarts.
- Cloud Music Library: enrolment and sync of the account's library in the engine.
- Catalogue playback through the app's own engine, with its software DRM path: **AAC 256 kbps**
  and **lossless (ALAC)**. The quality setting (`aac`, `lossless`, `hires`) is kept by the engine
  across restarts. Hi-Res Lossless is selectable but has not been verified.
- Playing a song in its album (next/previous follow the album), playing a whole album, pause,
  resume, stop, next, previous, seek, volume.
- Gapless transitions inside the engine's queue: the next item starts when the current one
  ends, without a new `/play` (continuous albums play without a dropout). "Play Next" of any
  catalogue song without interrupting playback (`/enqueue`), "Add to Queue"
  (`/enqueue` with `at=end`), removing queued items, the
  autoplay switch, and the next items in the status.
- Now-playing status: title, artist, album, genre, release date, catalogue song ID, position,
  duration, what is being decoded (format, sample rate, bit depth), and a flag when the engine
  stops responding.
- The audio stream shows up in the mixer as "Apple Music" with the current track as its name
  (PulseAudio / PipeWire).
- Push notification connection of the engine (certificate provisioning and connection work;
  whether pushes are delivered has not been verified).
- Repeated fast track switching without crashes or leaked audio streams.

Not working or not there yet: video, crossfade with lossless (AAC only, and only
for items queued early), push events in the API, reordering the queue, shuffle/repeat, lyrics and library
browsing through the API, playing a song inside a playlist's or search result's context. See
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## How it works

```
 your UI  (Sidra fork, a script, bin/amctl, ...)
    |
    |  HTTP + JSON on 127.0.0.1:7654, bearer token from a file in the prefix
    v
 +--------------------- Wine prefix (dedicated, never your default one) ------------------+
 |                                                                                        |
 |  amd.exe  (wrapper/, this project)                                                     |
 |    loads AMP.Services.dll  (the app's services layer, no WinUI)                        |
 |    + ampkg.dll, amshim.dll, amps.dll, amcertreq.dll  (shim/, this project)             |
 |         |                                                                              |
 |         |  COM (out of process)                                                        |
 |         v                                                                              |
 |  AMPLibraryAgent.exe  (the app's engine: library, playback, FairPlay, sign-in)         |
 |    + ampkg.dll  (package identity, audio fixes)                                        |
 |         |                                                                              |
 +---------|------------------------------------------------------------------------------+
           v
     winepulse  ->  PulseAudio / pipewire-pulse  ->  speakers
```

Details: [docs/architecture.md](docs/architecture.md) and [docs/shims.md](docs/shims.md).

## Quick start

Read [docs/install.md](docs/install.md) first; this is the short version. You need an x86_64
machine, 64-bit Wine with its development tools, a PE cross compiler for `winegcc` (clang with
lld, or mingw-w64), winetricks, Python 3, a C compiler and make, and the Apple Music MSIX
package. `./setup-prefix.sh --check` lists what is missing.

```sh
git clone https://github.com/Terrails/wine-engine-for-apple-music.git
cd wine-engine-for-apple-music
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
export AM_PYTHON="$PWD/.venv/bin/python"            # any python3 with dnfile works
./setup-prefix.sh /path/to/<the Apple Music .msix or .Msixbundle>
bin/amd-signin                 # native prompts: Apple Account, password, two-factor code
bin/amd                        # the service; stays in the foreground: use a second terminal
bin/amctl status
bin/amctl play <songId> <albumId>    # makes sound; IDs: docs/install.md, "First play"
bin/amctl quit
```

The prefix goes to `~/.local/share/wine-engine-for-apple-music/prefix` (XDG paths, see
[docs/configuration.md](docs/configuration.md)). `WINEPREFIX` is ignored on purpose: this never
touches your default Wine prefix.

## Documentation

| Document | What |
|---|---|
| [docs/install.md](docs/install.md) | requirements, the expected MSIX (version, checksums), build, setup, sign-in, first play, connecting a UI, updating, uninstalling |
| [docs/configuration.md](docs/configuration.md) | every environment variable, its default and its privacy impact |
| [docs/api.md](docs/api.md) | the local HTTP API |
| [docs/architecture.md](docs/architecture.md) | how the wrapper brings the app's services up, design notes |
| [docs/shims.md](docs/shims.md) | what each compatibility shim does |
| [docs/troubleshooting.md](docs/troubleshooting.md) | logs, sign-in failures, audio problems, debugging |
| [docs/development.md](docs/development.md) | building, the code generator, the mock service, experiments |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | open issues and workarounds |
| [CHANGELOG.md](CHANGELOG.md) | changes per version |

## Privacy

- Everything stays on your machine: the prefix, the engine's library and settings, and the logs
  under `~/.local/state/wine-engine-for-apple-music/logs`. The API listens on `127.0.0.1` only
  and requires a token that is regenerated on every start.
- **Logs are private.** Even at the default level, the service log contains the app's HTTP
  request lines to Apple, including cookies. Do not post logs unfiltered; when asking for help,
  share only the lines that matter and remove `Submitting the HTTP request` lines, cookies,
  tokens and account identifiers.
- Some settings log considerably more and must not be used for logs you intend to share:
  `AM_OSLOG_ARGS` (expands the app's private log arguments), `CFNETWORK_IO_LOG` /
  `CFNETWORK_IO_LOG_STDERR` (raw network I/O), `AMSHIM_HTTP_DUMP` (response bodies),
  `AM_TEST_USER` / `AM_TEST_PASS` (credentials in the environment), `AM_AGENT_OSLOG` with
  arguments, and raising the AuthKit sign-in log or the push service log above `error`: at
  `info`/`debug` they contain your Apple Account name, push tokens and user identifiers. See
  [docs/configuration.md](docs/configuration.md) and
  [docs/troubleshooting.md](docs/troubleshooting.md).
- The sign-in scripts run with minimal Wine tracing and without the app's private log arguments,
  so what you type in the prompts is not written to the log by the wrapper or the shims.
- The engine talks to Apple's servers as the app does on Windows (sign-in, library sync,
  streaming).

## Tested on

- CachyOS (Arch based), x86_64
- NVIDIA GPU (the engine does not use it for audio playback)
- Wine 11.21 and 11.22 (development releases)
- PipeWire with pipewire-pulse

Other distributions, Wine versions and audio setups are untested; reports are welcome.

## License

LGPL-2.1-or-later, see [LICENSE](LICENSE). This covers the code in this repository only. The
Apple Music app, its files and its metadata are Apple's and are not part of this project; you
supply them from your own copy.

## Disclaimer

Unofficial. Not affiliated with, endorsed by or sponsored by Apple Inc. Apple Music is a
trademark of Apple Inc. Windows and WinUI are trademarks of Microsoft
Corporation. Use of the Apple Music service is subject to Apple's terms.
