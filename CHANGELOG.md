# Changelog

All notable changes to this project are listed here. Versions follow
[Semantic Versioning](https://semver.org/); before 1.0.0, minor versions may break things.

## 0.1.0 (unreleased)

First public release. Supports Apple Music for Windows 1.1792.39941.0 (x64).

### Engine and service

- `amd.exe`, a wrapper service that loads the app's services layer (`AMP.Services.dll`) without
  its WinUI front end, brings it up in the order the app does, and drives the app's background
  engine `AMPLibraryAgent.exe` over COM.
- Sign-in to an Apple Account with two-factor authentication through native prompts
  (`bin/amd-signin`); the session persists in the prefix.
- The app's first-run privacy notice is accepted by the wrapper when started by the provided
  scripts (`AM_ACCEPT_WELCOME`), documented in `docs/install.md`.
- Cloud Music Library enrolment and sync.
- Catalogue playback, AAC 256 kbps and lossless (ALAC); quality setting `aac`, `lossless`,
  `hires`, kept by the engine across restarts.
- A song plays in its album (next/previous follow the album); album playback; pause, resume,
  stop, next, previous, seek, volume.
- Local HTTP + JSON API on `127.0.0.1:7654` with a per-start bearer token: `GET /status` (state,
  now-playing metadata, decoded format, engine-hang flag) and `POST` endpoints for playback
  control and quality (`docs/api.md`). `bin/amctl` as a command-line client.
- The audio stream is named "Apple Music" with `<artist> - <title>` as its stream name in
  PulseAudio/PipeWire mixers (`libampulse.so`).
- `PULSE_LATENCY_MSEC=100` by default against clicks under CPU load.
- The service ends the process directly on quit, avoiding a crash in the app's static
  destructors under Wine, and ends the prefix's Wine session after `/quit`.

### Compatibility shims

- `amshim.dll`: WinRT classes Wine lacks or stubs (HTTP on WinHTTP, XML on MSXML6, JSON, storage,
  settings persisted in the registry, dispatcher queues, resources, globalization helpers, the few
  WinUI types the services layer touches), generated from WinRT metadata with hand-written
  overrides.
- `ampkg.dll`: package identity for the unpackaged app, audio client fixes (no leaked streams,
  no signalled-after-close event handles, no permanent idle stream), the TLS client certificate
  fixes the push service needs, optional agent log.
- `amps.dll`: `IInspectable`/`IAsyncInfo` and `IAsyncOperation<HSTRING>` proxy/stubs.
- `amcertreq.dll`: the `X509_CERT_REQUEST_TO_BE_SIGNED` encoder missing from Wine's crypt32
  (push certificate provisioning).
- `webauthn.dll`, `bcp47mrm.dll`: small stubs.

### Setup

- `setup-prefix.sh` sets up a dedicated prefix in one run: verifies the MSIX (version and SHA-256,
  `.msix` or `.Msixbundle`; `AM_ALLOW_UNTESTED_MSIX=1` to try another version), unpacks it,
  patches the agent's imports and proxy DLL, registers its COM classes, installs the shims,
  downloads and verifies the Apple Root CA, disables Wine's Bluetooth driver and its desktop
  integration (`winemenubuilder`: no menu entries or file associations), and installs
  `vcrun2022`, `dxvk` and `sandbox` with winetricks. All downloads and builds happen before the
  first Wine command; on failure it names the step. Idempotent.
- `setup-prefix.sh --check` checks the requirements only (including a test compile with
  `winegcc`) and changes nothing.
- `tools/fetch-metadata.sh` downloads the Windows SDK and Windows App SDK metadata from nuget.org
  at pinned versions with SHA-256 checks. The interface table of the experiment program is
  generated at build time from your own copy of the app.
- XDG default paths for the prefix, logs and cache; `WINEPREFIX` is never used.
- `bin/` scripts work through symlinks, e.g. from `~/.local/bin`.

### Tooling

- `test/mock-daemon.mjs`: the API with a clock instead of audio, for testing UIs without Wine or
  sound.
- `dev/`: experiment program (`lab.exe`), agent run/debug helpers, a dropout monitor.
- Documentation: installation, configuration, API, architecture, shims, troubleshooting,
  development, known issues.
