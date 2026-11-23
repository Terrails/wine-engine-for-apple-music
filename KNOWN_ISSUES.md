# Known issues

Open issues of the current release, with workarounds where there are any. Background on how the
engine is driven: [docs/architecture.md](docs/architecture.md). Diagnosing problems:
[docs/troubleshooting.md](docs/troubleshooting.md).

## Playback and API

### A song plays in its album, not in the playlist or search result it was started from

`POST /play` takes a song ID and an album ID. The service opens the album in the app's page model,
queues the album and jumps to the song, so `next`/`previous` follow the album, not the list the
song was picked from. `queue=single` plays the song alone, after which the engine continues with
its own autoplay.

**Workaround:** keep the queue in the UI: `/play?...&queue=single` for the first item, then
`/enqueue` each next item while the current one plays (the engine continues into it by itself),
and `/dequeue` when the UI's queue changes. `/autoplay?on=0` keeps the engine's autoplay from
sounding after the last item.

### No push events, full queue, shuffle/repeat, lyrics or library in the API

The API offers playback control, a status snapshot and a little of the queue: the next five
items, Play Next (`/enqueue`), removing items and the autoplay switch. There are no events
(clients poll `GET /status`), no view of the whole queue, no reordering and no "add to the end"
(Add to Queue), no shuffle or repeat mode, no lyrics, and no access to the library or playlists.
The app has interfaces for these; they are not wired up. The app's own search is not used
either.

**Workaround:** poll `/status` (a few times per second is fine); take catalogue, library, playlists
and lyrics from another source, e.g. music.apple.com (the Sidra fork) or the public iTunes Search
API.

### `volume` in the status can be stale while nothing plays

With nothing playing, `GET /status` can report a volume that is not the one in effect (for
example `1.0` after `/volume?v=0`, or `0` after a restart although `1` had been set). Setting the
volume always works; only the reading is wrong. UIs that adopt the service's volume at start-up
can show the stale value.

**Workaround:** trust the reported volume only while a track is playing; set it explicitly before
playing if it matters.

### Crossfade has no effect

`/crossfade` sets the app's crossfade setting, which is stored, read back and kept across
restarts, but under Wine the engine never crossfades: with it on (6 s and 12 s) the outgoing
track played at full level up to its own end and the next one started after it, with one audio
stream at a time, the same as with crossfade off. Measured on album transitions, Play Next items
and autoplay. Why the engine does not mix the two tracks is not known.

**Workaround:** none; transitions are gapless or nearly so (next entry).

### Short silence between tracks of different albums

Tracks that follow each other in the engine's queue start without a new `/play`. Consecutive
tracks of an album in the same format are gapless (one audio stream, no dropout). Between tracks
of different albums (a Play Next item, autoplay) the engine replaces its audio stream: measured
0.66-0.91 s of digital silence including the first track's own fade-out tail, about 0.2 s of it
with no stream at all. A UI that starts every track with its own `/play` instead has a gap of
1-3 s (the album page loads first; about one switch in a dozen needs a second navigation, 3.5 s
or more).

**Workaround:** use `/enqueue` for the next track instead of `/play` (see
[docs/api.md](docs/api.md), "Queue and seamless playback").

### "Play Next" goes through the song's context menu

There is no direct call for queueing a catalogue song, so `/enqueue` opens the album page, builds
the track's context menu the way the app's UI does and runs its "Play Next" item. The menu items
carry no IDs; the item is found by its title "Play Next", or, in another UI language, as the item
after the one naming the track. Building the menu needs a clipboard check that Wine cannot
answer; the service redirects it (nothing is ever on the clipboard). A later app version may
change the menu.

**Workaround:** none needed with the supported version; a 500 with `hresult` `80004001` means
the menu had no Play Next.

### `/enqueue` blocks the other requests

Requests are handled one at a time on the engine's thread, and `/enqueue` loads the album page
like `/play` and then builds a context menu (typically 0.7-1.4 s, up to about 20 s; 404 after
about 13 s for a song that is not on the page). `/status` waits meanwhile.

**Workaround:** enqueue the next track 20-30 s before the current one ends, not at the last
moment, and allow for a slow status answer meanwhile.

### Audio clicks in long sessions are not ruled out

Short dropouts (a click and a moment of silence) may still occur in long listening sessions; this
has not been confirmed either way. The known causes are handled (the Bluetooth driver and the
audio buffer size, see [docs/architecture.md](docs/architecture.md), "Design notes").

**Workaround:** keep the default `PULSE_LATENCY_MSEC`. If you hear clicks, `dev/gapmon.py` logs
dropouts with the track position ([docs/troubleshooting.md](docs/troubleshooting.md)); please
report them.

### Hi-Res Lossless is not verified

`quality hires` is accepted and kept, but playback above 48 kHz has not been verified.
AAC 256 kbps and lossless (ALAC 16-bit/44.1 kHz) are verified. During the first 1-4 s of every
track the status reports `format` `other` and `sampleRate` `0` (the AAC variant), then the stream
switches; `bitDepth` is often empty for 16-bit and 24/48 lossless.

## Engine

### Intermittent sign-in error -7005

Sign-in occasionally fails with "There was an error connecting to the Apple Account server"
(AuthKit error -7005). That error means the HTTPS request to Apple failed at the transport level
(network, proxy or TLS); a wrong password gives a different message. The underlying cause of the
intermittent failures is unknown.

**Workaround:** run `bin/amd-signin` again. If it keeps failing, check the `didFail` line in the
AuthKit log ([docs/troubleshooting.md](docs/troubleshooting.md)) and that `setup-prefix.sh`
installed the Apple Root CA.

### `rootFeatureName.NotEmpty()` assertion on every play

`AMP.Services` logs a failed assertion `rootFeatureName.NotEmpty()` for every play request from
the wrapper, most likely because the app's UI normally passes a metrics feature name along with
the request. Playback is not affected.

**Workaround:** none needed; ignore the line.

### No video

Music videos cannot play through the engine: the video layer needs DirectComposition
(`AVCFPlayerLayerCreateWithAVCFPlayerForDirectComposition`), and Wine has only a stub
`dcomp.dll`. Audio playback does not use it.

**Workaround:** play videos elsewhere. The Sidra fork leaves music videos to the web page's own
player.

### No spatial audio device, no hardware DRM capabilities

The app's media stack asks once for `Windows.Media.Audio.SpatialAudioDeviceConfiguration`, which
Wine does not have, and carries on without a spatial audio device. Its `WMFCapabilities.exe` probe
fails under Wine (no `IMFExtendedDRMTypeSupport`), so the engine assumes no hardware DRM and no
audio passthrough. Both match what Wine offers; only video (PlayReady) would need more.
Dolby Atmos / spatial audio output has not been tested.

**Workaround:** none.

### App settings stored through CFPreferences are per executable

Settings the app keeps in its CFPreferences (for example the agent's streaming quality in
`AMPLibraryAgent.exe.plist`) are stored per program name. `amd.exe` and the experiment program
`lab.exe` therefore have separate app-side preferences. Settings stored through `ApplicationData`
are shared by all programs in the prefix (the shim keeps them in the registry).

**Workaround:** none needed for normal use; keep it in mind when comparing `lab.exe` runs with the
service.

### Some `ApplicationData` settings are not persisted

The shim persists strings, numbers, booleans and the other scalar types. Arrays,
`Point`/`Size`/`Rect` and `ApplicationDataCompositeValue` are kept in memory only (logged as
`fixme:amshim:settings_write`), and `ApplicationData.ClearAsync` does nothing. None of these has
been seen in use.

**Workaround:** none needed so far.

### Push notification delivery is unverified

The engine's push client provisions its certificate and connects to Apple's push service,
filters its topics and keeps the connection alive. Whether pushes actually arrive and are acted
upon (for example library changes made on another device) has not been checked.

**Workaround:** if changes made elsewhere do not show up, restart the service; the engine checks
its cloud library when it starts.

### Only one app version is supported

The wrapper and `ampkg.dll` are written against Apple Music for Windows 1.1792.39941.0: the
package identity `ampkg.dll` reports is fixed to that version, and the wrapper calls the app's
interfaces by vtable slot and interface ID taken from it. `setup-prefix.sh` therefore accepts only
that package (checksum checked). Reading the identity from the package's `AppxManifest.xml` at run
time would remove one of these ties; the vtable slots would still need checking for each new
version.

**Workaround:** `AM_ALLOW_UNTESTED_MSIX=1` accepts another version at your own risk
([docs/install.md](docs/install.md)).

### The service may exit with `FATAL exception 80000003`

The app has a race in its store bag code (a dictionary finalized twice). It has not been seen
since the shim returns proper cache headers, but it is still in the app and can end the service.

**Workaround:** start the service again.

### App files with special characters keep their encoded names

The package stores some file names percent-encoded, and `setup-prefix.sh` unpacks them with
`unzip`, which keeps them that way (about 20 files, e.g. `Black%20%26%20White.icc`,
`MetadataBadge_HDR10%2B.svg`); the Windows installer would decode them. The package's own
bookkeeping files (`AppxManifest.xml`, `AppxBlockMap.xml`, ...) are copied into the app folder as
well, and a refresh with another package version keeps files the new version no longer has.
Nothing has been seen to fail because of it.

**Workaround:** none needed so far. For a clean app folder, delete
`$AM_PREFIX/drive_c/Program Files/AppleMusic` and run `setup-prefix.sh` again.

## Scripts

### The "is it running" checks see every prefix

`setup-prefix.sh`, `shim/install.sh`, `bin/amd-signin` and `dev/*.sh` refuse to start while
`amd.exe` or `AMPLibraryAgent.exe` runs, and after `/quit` `bin/amd` waits for the agent to exit
before it ends the Wine session. The check (`pgrep -f`) matches any process whose command line
contains those names: a service or agent in another Wine prefix, `winedbg` started by
`dev/dbg-agent.sh`, or an editor or `grep` with that name on its command line.

**Workaround:** if a script refuses although nothing of this project runs, look at
`pgrep -af 'AMPLibraryAgent|amd\.exe'` and end or wait for what it lists.

### A quick restart right after `/quit` can be ended by the old instance

After `/quit`, `bin/amd` waits up to 20 s for the agent to exit and then ends the prefix's Wine
session. If a new `bin/amd` (for example started by a UI that found the port closed) starts in
that window, the old one sees the new agent, keeps waiting, and its final `wineserver -k` ends
the new session. Two `bin/amd` started at the same time can likewise end each other's session.

**Workaround:** wait until the previous `bin/amd` has exited before starting the service again;
if the new one was ended, start it once more.

## For UI authors

### Keep the clicked track and retry after a restart

A UI should keep showing the track the listener picked until `/status` reports it (loading takes
1-3 s), and if the service was restarted in between (connection refused, new token), send the
`/play` again once it is ready. This is recommended behaviour; it is not implemented in every
client yet (`bin/amctl` does neither).

### One player at a time

The service has one engine and no notion of clients. Any UI or `bin/amctl` that sends a command
controls the same playback; UIs that assume they own playback (the Sidra fork does) will stop or
pause what another client started.
