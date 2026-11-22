# Architecture

This project runs the engine of Apple Music for Windows (the MSIX package
`AppleInc.AppleMusicWin`) under Wine, without its user interface. The WinUI 3 front end cannot
run under Wine (it needs in-process ALPC and a working DirectComposition), so you bring a UI and
talk to the engine through a small local HTTP API ([api.md](api.md)).

This page describes the processes, the start-up sequence, how the app's own object model is
driven, sign-in, audio, where state is kept, and the reasons behind the less obvious parts.
Installing is covered in [install.md](install.md), settings and environment variables in
[configuration.md](configuration.md), open problems in [KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

## Components and processes

```
 your UI (Sidra fork, a script, ...)
        │  HTTP/1.1 + JSON, 127.0.0.1:$AM_PORT (7654), Bearer token
        ▼
┌─ Wine session of $AM_PREFIX ─────────────────────────────────────────────────────────┐
│                                                                                       │
│  amd.exe  (wrapper/src, "the service")                                                │
│   ├─ ampkg.dll          package identity, Wine fixes            (shim/ampkg)          │
│   ├─ AMP.Services.dll   the app's services layer + its deps                            │
│   │    (AMP.Utils, AuthKitWin, CoreFoundation, CFNetwork, ...)  ← from the MSIX        │
│   ├─ amshim.dll         WinRT classes Wine lacks                (shim/, via registry)  │
│   ├─ webauthn.dll       stub, probed by AuthKit during sign-in  (shim/webauthn)        │
│   └─ HTTP server thread → requests run on the main (UI) thread                         │
│        │                                                                               │
│        │ COM (out of process): CLSID {68E7097C-…} IAMPLibrary                          │
│        ▼    proxies: AMPLibraryAgent.Proxies.dll + amps.dll (shim/ps)                  │
│  AMPLibraryAgent.exe  -Embedding   (library, playback, FairPlay, cloud library, push)   │
│   ├─ ampkg.dll          first import (added at setup by tools/addimport.py)           │
│   ├─ amshim.dll, amcertreq.dll (push service certificate request)                     │
│   └─ audio: mmdevapi → winepulse.so ─────────────┐                                    │
└──────────────────────────────────────────────────│────────────────────────────────────┘
                                                   ▼
                          libampulse.so (LD_PRELOAD, names the stream)  →  PulseAudio / PipeWire
```

- **`amd.exe`** is the wrapper service, built from `wrapper/src` with winegcc. `bin/amd` copies
  it next to the app in `C:\Program Files\AppleMusic` and runs `amd.exe serve [port]` in the
  prefix. It loads `AMP.Services.dll`, the same services layer the real `AppleMusic.exe`
  uses, brings it up the way the real executable does (next section), and answers the local
  API. `amd.exe signin` runs the same start-up and then an interactive sign-in
  (`bin/amd-signin`).
- **`AMPLibraryAgent.exe`** is the app's own out-of-process agent. AMP.Services starts it
  through COM (`CoCreateInstance` of `IAMPLibrary`; `LocalServer32` =
  `AMPLibraryAgent.exe -Embedding`). The registration comes from the package manifest and is
  written by `setup-prefix.sh` from the package's own `AppxManifest.xml`
  (`tools/genagentreg.py`) and imported, because nothing installs the MSIX under Wine. The
  prefix's copy of the agent has `ampkg.dll` added as its first import
  (`tools/addimport.py`), so the shim is loaded before any of the app's DLLs initialise.
- **Shims** fill Wine gaps ([shims.md](shims.md)): `ampkg.dll` (package identity and fixes by
  import patching), `amshim.dll` (WinRT runtime classes, registered over Wine's), `amps.dll`
  (proxy/stubs Windows ships and Wine does not), `amcertreq.dll` (a crypt32 encoder),
  `webauthn.dll` and `bcp47mrm.dll` (small stubs; the latter for the app's resource loader,
  MRT Core).
- **`libampulse.so`** is a Linux library (`wrapper/pulse/ampulse.c`), not a Windows DLL.
  `bin/amd` preloads it into the Wine session to give the agent's audio stream the name of the
  track (see "Audio").

The service is single-instance per prefix and is meant to be the only player: several UIs may
read its status, but one UI controls playback at a time (see "Design notes").

## Start-up order

`app_start()` in `wrapper/src/app.c` performs these steps in this order, mirroring what
`AppleMusic.exe` and WinUI do. Where a step's failure mode is known, it is given.

1. **`LoadLibrary("ampkg.dll")`, then `RoInitialize(STA)`.** ampkg must be in place before any
   of the app's DLLs ask for their package identity (they fail with "no package" otherwise).
   The main thread is the app's UI thread, a single-threaded apartment; the app's C++/WinRT
   code resumes on it with `co_await` on the UI context.
2. **Marshal a dummy object once.** Wine starts an apartment's remoting (and its `IRundown`
   stub) only when the apartment first marshals something. Until then, C++/WinRT's
   `co_await ui_context` reaches the STA through `IContextCallback` → `IRundown::DoCallback`
   and fails with `0x800706ba` (`RPC_S_SERVER_UNAVAILABLE`).
3. **`Microsoft.UI.Dispatching.DispatcherQueueController.CreateOnCurrentThread()`** (from
   amshim). WinUI's `Application.Start` would create this dispatcher queue; AMP.Services posts
   work to it.
4. **`OSLogInitialize("AppleMusic")`** from `AMP.Utils.dll`, as `AppleMusic.exe` does first
   thing in `wWinMain`. Just before, the wrapper loads `AMP.Services.dll` and installs its
   hooks: the app's log on stderr (`oslog.c`) and the AuthKit UI provider (`akui.c`). C++
   exception and crash reporting (`cxxdbg.c`) is installed first thing in step 1.
5. **`ServiceProvider.InitializeAppContext`, `Initialize(false, factory)`, `AutoStartAsync`.**
   `Initialize` requires a non-null `ICustomPageViewModelFactory` (the UI's XAML page factory);
   the wrapper passes one that creates nothing.
6. **Install the wrapper's `IDialogServiceImpl`** (`DialogService.Implementation`, after a
   query for `IDialogService` `{4e87acf0-…}`). Sign-in and message boxes go through it. The
   object must support `IWeakReferenceSource`: DialogService keeps only a weak reference to its
   implementation and assumes it can get one.
7. **`MainPageViewModel.TryLoadAsync(sp)`, `NavigatedTo()`, accept the first-run notice.** On a
   new prefix the app shows a welcome/privacy notice (`'gdpw'` notification,
   `GDPRWelcomeDialogViewModel`). Only after it has been accepted does the app launch the agent
   and start its library, playback and cloud library services. `bin/amd` and `bin/amd-signin`
   set `AM_ACCEPT_WELCOME=1`, which runs the notice's `AcceptAction` (slot 12). The app stores
   the answer in its LocalSettings (`GDPRWelcomeDialog_ShownContentVersion`), so later starts
   skip it.
8. **Pump messages on the main thread** (`pump()`): window messages, queued dialogs, API
   requests and the now-playing name, all on this one thread.

`serve` then waits up to 30 s for the playback service to report started, opens the play queue
("Queue" below), starts the HTTP server and writes the token. If no working agent turns up
it exits with code 4.

Exit codes of `amd.exe`: 0 normal (after `POST /quit`), 1 start-up failed, 2 bad arguments,
4 the agent is gone. On `/quit` the service stops playback and ends its process directly with
`TerminateProcess`: a normal exit runs the app DLLs' static destructors, and
AppleMediaServicesKit's teardown crashes under Wine. Nothing is left to save at that point.

## The app's model, as the wrapper drives it

AMP.Services has no public headers. The wrapper calls its WinRT interfaces by vtable slot
(`VT`/`CALL0`/`CALL1` in `wrapper/src/common.h`); slot = 6 + method index, and `tools/q`
prints the slots of any interface in the app's metadata ([development.md](development.md)).
Slots below are those of the tested package version.

### Actions and notifications

- **Actions are opaque objects; you run one by posting a `'disp'` notification** (fourcc
  `0x64697370`): the `NotificationArgs` factory `{9b1e00ef-…}` slot 6 creates
  `NotificationArgs('disp', action)`, then `NotificationCenter.PostAsync(args)`
  (ServiceProvider slot 32, method slot 6). `dispatch_action()` in `wrapper/src/page.c`.
- Requests from the app to its UI arrive as NotificationCenter events (`add_Notification`,
  slot 7; the event kind is a fourcc). The wrapper subscribes to the NotificationCenter and the
  PageNotificationCenter (slots 32, 33) and logs them (`dialogs.c`).

### ServiceProvider

| Slot | Member |
|---|---|
| 6 | `Initialize(bool, ICustomPageViewModelFactory)` |
| 11 | `AutoStartAsync` |
| 16 / 18 | `AgentPhase` / `AgentIsHung` (the agent watchdog, see [api.md](api.md)) |
| 20-26 | services: Activity, AgentDebug, Artwork, CloudLibrary, Library, Playback, VideoPlayback |
| 27 | AppSettings (→ slot 8 `IPlaybackSettings`: streaming quality) |
| 28 | DialogService |
| 31 | Navigation (→ slot 8 RootStack → slot 6 CurrentViewModel) |
| 32 / 33 | NotificationCenter / PageNotificationCenter |
| 34 | AccountViewModel (`IAccountViewModel` `{947f2669-…}`: 11 SignedIn, 12 IsEnrolledInMusic, 16 `SignInAsync`) |

`MainPageViewModel`: 16 `IsPlayQueueOpen` (put), 30 `NavigatedTo`, 31 `ProcessDeeplinkAsync`;
it also exposes NowPlaying, Sidebar, Transport, Volume and MiniPlayer view models.

### Content and pages

- Catalogue items are identified by a `ContentDescriptor`; its `AdamId` (slot 21) is the
  catalogue ID. The JSON form is
  `[{"kind":"album","identifiers":{"storeAdamID":"<albumId>"}}]` (`"song"` likewise), which
  `DeserializeContentDescriptors` accepts.
- A page (`IContentPageViewModel`: 9 error, 14 IsLoading, 18 page) has sections
  (`IPage.HasSectionAtIndex`/`TryGetSectionAtIndex`, 30/31); a section is an
  `IComponentModelContainer` with `Items`. Items answer `ITitledComponent`, `IPlayActionDriven`
  (their play action) and `IContentDescriptorDriven`. `page.c` walks them.

### How `/play` works

There is no "play catalogue item by ID" call (`IPlayer.StartOrQueueLibraryPlaybackAsync` takes
library items only), so the wrapper does what a link does (`wrapper/src/play.c`):

1. Open the album page by deep link:
   `MainPageViewModel.ProcessDeeplinkAsync("musics://music.apple.com/<cc>/album/<albumId>?i=<songId>", false)`.
   The page model navigates (typically 0.4-0.8 s).
2. Find the actions on the page: the header item (`AlbumDetailHeaderLockup`, AdamId = album ID)
   whose action queues the whole album, and the song's row (`AlbumTrackLockup`, AdamId = song
   ID) whose action queues that track alone, followed by autoplay. The track list is complete
   long before `IsLoading` clears (which also covers social and library extras), so the song
   is looked for right away. About one navigation in a dozen ends on a loaded page without the
   item; the wrapper navigates again (log `item not on the page after … ms, navigating
   again`). It gives up after 10 s (HTTP 404).
3. **Song in its album** (the default): run the header's action, wait until the song appears
   among the *new* play-queue items (IDs above the highest one before), then jump to it with
   `PlayQueue.PlayPlayQueueItemAsync(id)` (`IPlayQueue` slot 13). The jump comes 0.3-0.6 s
   after the album starts, while the player is still at 0 ms of the first track, so nothing
   of it is heard. Next/previous then follow the album. If the song turns up in the
   *currently playing* section (`IPlayQueueSection.SectionKind` 2: the album's first track,
   or a single), there is no jump: jumping to the item that is loading leaves the player
   stopped at 0 ms. If the song is not in the queue after 5 s, the row's own action is used.
4. `queue=single` runs the row's action (the song, then the app's autoplay). An album without
   a song uses `IPage.PlayAllAction` (slot 25, usually null; `UpdatePlayAllAction` slot 32 is
   tried) or else the header's action.

**Queue.** `PlayQueue.Current` → `IPlayQueue.Sections` (slot 9) stays empty until the "Up Next"
panel is open. `queue_open()` (`wrapper/src/queue.c`) does what opening the panel does: sets
`MainPageViewModel.IsPlayQueueOpen` and keeps a `PlayQueueViewModel` alive for the life of the
process. Sections seen: now playing, the album ("up next"), autoplay.

**Player.** `Player.Instance` (`IPlayer`): 6 NowPlayingItem, 7 PlayerState, 12 Play, 13 Pause,
14 Stop, 16 SkipPrevious, 17 SkipNext, 19/20 shuffle/repeat mode, 22
`SetPositionAsync(TimeSpan, bool)`, 27 SetVolume, 28 SetMute. `IPlayerState`: 21 Volume,
24 IsPaused, 25 IsPlaying. `INowPlayingItem`: 6 Subtitle, 7 Artist, 8 Album, 11 Artwork,
18 ISOReleaseDate, 19 Name, 21 Genre, 24 IsPlaceholder, 30 CurrentPosition, 31 Duration,
38 AudioFormat, 39/40 sample rate / bit depth (strings), 45 PlayQueueItemID. For catalogue
items Artist and Album are empty; the Subtitle reads `<artist> — <album>` and the wrapper
splits it. Quality: `IPlaybackSettings` 21/22 `LosslessAudio`, 23/24
`StreamingAudioQuality` (1 AAC 256, 2 lossless up to 48 kHz, 3 hi-res lossless up to
192 kHz).

## Sign-in and AuthKit

- `bin/amd-signin` runs `amd.exe signin`, which calls `AccountViewModel.SignInAsync(true)`. The
  app then calls the wrapper's `ShowAuthDialogAsync` with a `SignInHandler`. `signin.c` shows a
  native prompt (`prompt.c`: a plain Win32 window, credentials never logged), calls
  `SignInHandler.Invoke(NULL, user, password, NULL)` → `SignInStatus` (0 success, 1 failure,
  2 silent failure) and completes the dialog with an `AuthResult`. A failure asks again with
  the name kept.
- The sign-in runs **in the wrapper process**: AuthKitWin → CFNetwork → Wine's secur32/schannel.
- Interactive steps (two-factor code, alerts) go through an `AKUIProvider`, a C++ class
  AuthKitWin exports and the real UI subclasses. `akui.c` constructs one with the base class
  constructor and swaps in its own vtable (slot 1 parent window, 3 alert, 4/5 server-provided
  web UI, 6 blocking "get 2FA code", 7 set code). AMP.Services installs the UI's provider with
  `SetUIProvider()` before authenticating; the wrapper redirects that import so its own
  provider stays installed. Server-provided web UI (which needs a WebView) is not supported and
  only logged.
- Apple's sign-in server (`gsa.apple.com`) chains to the **Apple Root CA**, which Linux CA
  bundles (and so Wine) do not contain. Without it the TLS connection is dropped before
  anything is sent. `setup-prefix.sh` downloads the certificate, checks its SHA-256 and adds it
  to the prefix's machine Root store with `tools/addroot.c`.
- The session persists in the prefix; signing in is needed once.
- AuthKit's own log is turned on at error level; see [troubleshooting.md](troubleshooting.md).

## Audio

The agent plays through Wine's mmdevapi and `winepulse.so` to PulseAudio or PipeWire's pulse
server; nothing in this project touches the decoded audio.

- **Names in the mixer.** Wine names the client after the executable's product name
  ("AMPLibraryAgent"); `bin/amd` overrides it through libpulse's environment
  (`PULSE_PROP_OVERRIDE_application.name`, `AM_AUDIO_NAME`). The stream gets the track:
  `wrapper/src/medianame.c` writes `<artist> - <title>` (empty when nothing is loaded) to
  `%LOCALAPPDATA%\AppleMusicWrapper\media-name` every 500 ms, replacing the file atomically.
  `libampulse.so`, preloaded by `bin/amd` with the file's Linux path in `AM_MEDIA_NAME_FILE`,
  wraps winepulse's `pa_stream_new` and its mainloop poll function and sets the stream's
  `media.name` from the file (`pa_stream_set_name`, at most every 250 ms, under winepulse's
  lock), or leaves Wine's own name when it is empty. Some mixers hide media names they consider
  generic (KDE Plasma hides names matching `playback|audio|stream|alsa|pulse|pipewire`), so an
  occasional title shows as the client name only. `AM_AUDIO_TRACK_NAME=0` turns the preload off.
- **Buffering.** `bin/amd` sets `PULSE_LATENCY_MSEC=100` unless you set it (see "Design
  notes").
- **One stream.** The agent's silent start-up stream is answered by ampkg without a real stream,
  so only a loaded track has a stream ([shims.md](shims.md), ampkg).
- Video output is not available (no DirectComposition); audio does not need it.

## Where state is kept

Everything lives inside `$AM_PREFIX` (see [configuration.md](configuration.md) for the default
location); logs go to `$AM_LOG_DIR`.

| What | Where (inside the prefix) | Written by |
|---|---|---|
| API token, now-playing name | `%LOCALAPPDATA%\AppleMusicWrapper\{token,media-name}` | `amd.exe` |
| App settings (`ApplicationData` Local/Roaming and child containers) | `HKCU\Software\AppleMusicWrapper\Settings\{Local,Roaming}` | amshim, for every process of the prefix |
| Agent and app preferences (e.g. the streaming quality, `preferredStreamPlaybackAudioQuality`) | CFPreferences, one file per executable name: `%APPDATA%\Apple Computer\Preferences\<exe>.plist` (`AMPLibraryAgent.exe`, `amd.exe`, `lab.exe`) | the app's CoreFoundation |
| AuthKit log level | `HKCU\Software\Apple Inc.\Internet Services\Logging` | `amd.exe` sets `amd=error` if unset |
| Library | `Music\Apple Music` in the prefix's user profile (see "The prefix is sandboxed") | the agent |
| Caches and logs of the app's components (e.g. AuthKit) | `%LOCALAPPDATA%\Packages\AppleInc.AppleMusicWin_nzyj5cx40ttqa\LocalCache` | the app |
| Sign-in session | the app's own storage in the prefix | the app |
| Push service certificate | `HKCU\Software\Microsoft\SystemCertificates\My` | the push service |

The in-prefix names `AppleMusicWrapper` are internal and kept for compatibility with existing
prefixes. Because CFPreferences follow the executable name, `amd.exe` and `lab.exe` keep
separate app preferences, while the ApplicationData settings (registry) are shared.

## Wrapper source files

| File (`wrapper/src/`) | What |
|---|---|
| `main.c` | `amd.exe serve [port]` / `amd.exe signin`, exit codes |
| `app.c` | start-up sequence above, `pump()` (message loop, queued dialogs, API requests) |
| `play.c` | `/play`: album page by deep link, song in its album (album action + queue jump) |
| `page.c` | page model: deep links, walking a page's or container's items, `dispatch_action()` |
| `queue.c` | the play queue: open it, find an item, jump to it |
| `player.c` | `IPlayer`, playback settings (quality), status snapshot |
| `api.c` / `server.c` | API endpoints / HTTP server, token, JSON helpers |
| `dialogs.c` | the `IDialogServiceImpl`, the first-run notice, NotificationCenter subscription |
| `signin.c`, `akui.c`, `prompt.c` | sign-in flow, AuthKit UI provider (2FA, alerts), native prompt window |
| `oslog.c`, `cxxdbg.c` | the app's log on stderr (`oslog[n]:`, `asl[n]:`), C++ throws and crash backtraces |
| `medianame.c` | now-playing name for the mixer |
| `async.c`, `winrt.c`, `iat.c`, `log.c` | async objects, WinRT helpers, import patching, logging |
| `common.h` | `VT`/`CALL0`/`CALL1` vtable calls, shared declarations |
| `../pulse/ampulse.c` | `libampulse.so` (Linux, LD_PRELOAD): stream name from the media-name file |

`dev/lab/lab.c` builds `lab.exe` from the same sources minus `main.c`, for experiments
([development.md](development.md)). Wrapper log lines start with `wrapper: `.

## Design notes: why it works this way

### Song-switch crashes: three causes

Switching songs quickly used to kill the agent or the service after 5-30 switches. There were
three separate causes; all are fixed.

1. **Audio event handle signalled after close** (`shim/ampkg/audio.c`). Wine's mmdevapi timer
   thread calls `SetEvent` on the handle passed to `IAudioClient::SetEventHandle` until the
   client is destroyed. The app closes its handle earlier, which is fine on Windows because the
   audio engine holds its own reference. Under Wine the handle value was reused, typically by
   combase for the completion event of an outgoing COM call, and the stray `SetEvent` woke that
   call before its reply arrived (`unmarshal_ORPCTHAT invalid flags`, then a null dereference).
   ampkg patches Wine's `IAudioClient` vtable (found through a hooked
   `CoCreateInstance(MMDeviceEnumerator)`) and gives Wine a duplicate of the handle, closed when
   the client is released.
2. **One leaked audio client per switch** (same file). The app never releases the
   `IAudioSessionControl` it gets from `GetService`. In Wine that object holds a reference on
   the audio client (on Windows it does not), so no client was ever destroyed: one more
   PipeWire stream and timer thread per track. ampkg makes that reference weak: when the app
   drops its last own reference the client is released for real, and the orphaned session
   control's final release is swallowed.
3. **CoreFoundation HALT in the service after about five minutes** (`FATAL exception 80000003`,
   which also took the agent down). A dictionary was finalised twice in the store-bag code
   during a bag refresh, a race in the app, and the refresh ran about twice a second:
   `HttpResponse` freshness comes from `Headers.CacheControl.MaxAge`, else
   `Content.Headers.Expires`, else `LastModified`, else "expires now", and amshim returned
   nothing for all three. `shim/ov_httpheaders.c` implements them; the bag is now fetched once.
   The race is still in the app; it is just no longer exercised hundreds of times. The
   backtrace that found it came from `wrapper/src/cxxdbg.c`.

After the fixes: 144 switches in 9 minutes (six songs in turn every 2.5 s) without a failed play
or a dead process.

### No idle audio stream

At start-up the agent opens a shared, event-driven audio client and writes silence into it for
as long as it runs; music uses a separate client per track. On Linux that was a permanent
second playback stream. ampkg (`shim/ampkg/audio_idle.c`) answers that first client itself
(Initialize, GetBufferSize, GetStreamLatency, GetCurrentPadding, Start/Stop/Reset, proxy
`IAudioRenderClient` and `IAudioStreamVolume`, a 10 ms thread signalling the event) and creates
no Wine stream. If non-silent audio is ever written to it, or another service is requested, the
real stream is created on the spot (log `ampkg: idle audio client … now has a real stream`).
`AM_AUDIO_IDLE_STREAM=1` restores the original behaviour. Result: no stream while idle, one
while a track is loaded.

### winebth.sys is disabled

Playback dropped out for a fraction of a second at regular intervals (seconds under load). The
cause was wineserver saving the registry: Wine's Bluetooth driver records every device the host
sees under `HKLM\System\CurrentControlSet\Enum\WINEBTH` and never forgets one. With random
address BLE devices around and constant scanning, one prefix had recorded over two thousand
devices, 43 MB of `system.reg`. wineserver rewrites the registry every 30 s when HKLM changed
and blocks every Wine thread meanwhile (about 0.4 s idle, up to 3.4 s under load), which empties
the ~100 ms of audio buffers. The agent needs no Bluetooth, so `setup-prefix.sh` disables
`winebth.sys` with a DLL override (setting `Services\winebth` `Start=4` does not stop it, it is
a PnP bus driver) and deletes the recorded devices. A registry save then costs well under 50 ms.

### PULSE_LATENCY_MSEC=100

Each track's audio client asks for a 10 ms shared-mode buffer; Wine makes it 30 ms with a 10 ms
period, and opens the pulse stream with a target length (`tlength`) of 25 ms. pipewire-pulse
turns that into a 256-frame quantum (5.3 ms) that also pulls the whole graph down. That leaves
about 20 ms of slack: a late Wine push thread means a click. Measured on a null sink with
`dev/gapmon.py` for 120 s under a CPU load of two busy loops per core (no load: no gaps):

| Setting | Gaps per run |
|---|---|
| default (pulse target 25 ms, Wine buffer 30 ms) | 10, 70 |
| Wine buffer 100 ms only (pulse target still 25 ms) | 79 |
| `PULSE_LATENCY_MSEC=60` (target 45 ms) | 0, 2, 11, 0 |
| `PULSE_LATENCY_MSEC=100` (target 70 ms) | 0, 0 |

So the agent fills fast enough; what runs dry is the pulse side. `bin/amd` sets 100 unless
you set it. It costs about 70 ms of output delay, which nothing here needs to avoid, and it
stops the stream from forcing the whole PipeWire graph to 256-frame cycles.

### NdrStubCall3 is renamed to NdrStubCall2

`AMPLibraryAgent.Proxies.dll` (the proxy/stub DLL for `IAMPLibrary`) imports
`rpcrt4!NdrStubCall3`, which Wine does not export. `NdrStubCall2` has the same signature and
the DLL carries DCE format strings, so `setup-prefix.sh` patches the import name in the
prefix's copy. The IInspectable, IAsyncInfo and `IAsyncOperation<HSTRING>` proxy/stubs the
agent's interfaces build on come from `amps.dll` ([shims.md](shims.md)).

### The dialog implementation needs IWeakReferenceSource

DialogService keeps only a weak reference to its implementation and assumes it can get one, so
an implementation that does not answer `IWeakReferenceSource` does not work (and without a
dialog implementation there is no sign-in). The wrapper's implementation is a static singleton
whose weak reference resolves to itself (`wrapper/src/dialogs.c`).

### One player at a time

The engine has one queue and one now-playing item. Clients do not own playback: whatever the
engine plays is taken to belong to the UI in front of you. The Sidra fork, for example, pauses
the engine when its page is reloaded and stops it when it quits, whoever started the track.
Several players controlling the engine at once are not supported; several readers of
`/status` are fine.

### WINEPREFIX is ignored

The scripts use `AM_PREFIX` and set `WINEPREFIX` themselves, so this project never touches your
default Wine prefix. Many setups export `WINEPREFIX` for another prefix, and `bin/amd` restarts
the Wine session of its prefix (`wineserver -k`) before it starts: honouring `WINEPREFIX` would
end the programs you run there.

### Why the Wine session is restarted

If the agent was killed or crashed, Wine's rpcss keeps its stale COM class registration and the
next start gets a dead object (creating `IAMPLibrary` fails with `0x800706ba`, nothing plays).
So `bin/amd`, `bin/amd-signin` and `dev/lab.sh` end everything running in the prefix first,
which is why the prefix is dedicated to this app. `AM_KEEP_WINESERVER` (set to any value) skips
that at the start, and in `bin/amd` also after `/quit`.

### The prefix is sandboxed

`setup-prefix.sh` runs `winetricks sandbox`: the prefix's Documents, Music and similar folders
stop being links into your home directory, and the `Z:` drive (your whole file system) is
removed. The agent creates its library under `Music\Apple Music`; in an unsandboxed prefix
that is your real `~/Music`. As a consequence Wine tools inside the prefix only see `C:` paths:
the scripts copy files into `C:\windows\temp` before importing them.

### Desktop integration is off

`setup-prefix.sh` disables `winemenubuilder.exe`, through `WINEDLLOVERRIDES` for every Wine program
it starts and as a disabled DLL override in the prefix for later runs. Otherwise Wine would add
menu entries, `wine-extension-*.desktop` files and MIME associations for this prefix to your
desktop: the app cannot be started from a menu here (its front end does not run under Wine),
and files of its types should not open in a prefix that exists only for the engine.
