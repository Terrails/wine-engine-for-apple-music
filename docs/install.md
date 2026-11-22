# Installation

This guide takes you from nothing to music playing. It assumes you have never seen the project.

1. [Requirements](#requirements)
2. [The Apple Music package](#the-apple-music-package)
3. [Clone and build](#clone-and-build)
4. [Set up the prefix](#set-up-the-prefix)
5. [Sign in](#sign-in)
6. [First play](#first-play)
7. [Putting the commands on your PATH](#putting-the-commands-on-your-path)
8. [Connecting a UI](#connecting-a-ui)
9. [Updating](#updating)
10. [Uninstalling](#uninstalling)

What you need besides your computer: an **Apple Account with an Apple Music subscription**, and
**your own copy of the Apple Music for Windows package** (below). Nothing from the app ships with
this project.

## Requirements

Minimum, in short:

- An **x86_64** (64-bit Intel/AMD) machine: the app exists for x64 only (no ARM, no 32-bit).
- A **64-bit Wine**. Tested with 11.21 and 11.22; there is no known minimum version, but anything
  older than 11.21 is untested.
- A **Vulkan driver** for your GPU: setup installs DXVK, which implements Direct3D on Vulkan.
  Audio playback does not draw anything, and a machine without Vulkan has not been tested.
- **About 2 GB of disk space**: the prefix takes about 1.2 GB, the cache (unpacked package and
  metadata) about 0.5 GB, plus winetricks' downloads (about 60 MB) and the package file itself
  (about 210 MB).

| Component | Needed for | Version |
|---|---|---|
| Wine, 64-bit (WoW64 or a classic 64-bit build) | running the app and the wrapper | tested: 11.21, 11.22 (development releases). Older versions are untested. |
| Wine development tools: `winegcc`, `winebuild`, `widl` and the Wine headers | building the wrapper and the shims | the same version as your Wine |
| `clang` with `lld`, or `mingw-w64-gcc` | the PE cross compiler `winegcc` uses to build x86_64 Windows binaries (not part of Wine) | any recent version |
| Wine's `winepulse` audio driver | audio output | part of a normal Wine build with PulseAudio support |
| GnuTLS (Wine's TLS backend) | every HTTPS connection of the app (sign-in, catalogue, playback) | any version your Wine was built against |
| PulseAudio, or PipeWire with `pipewire-pulse` | audio output | tested: PipeWire with pipewire-pulse |
| libpulse development headers (`pulse/stream.h`) | `libampulse.so` (names the audio stream after the track) | any recent version |
| A C compiler (`cc`) and `make` | `libampulse.so`, the build | any recent version |
| `winetricks` | installs `vcrun2022`, `dxvk` and the `sandbox` setting into the prefix | tested: 20260125. Old distribution packages may fail to download components. |
| Python 3 with `dnfile` and `pefile` (`requirements.txt`) | the WinRT metadata code generator and the PE patching tools | tested: Python 3.14 with the pinned dnfile 0.18.0 and pefile 2024.8.26. Older Python versions are untested. |
| `unzip`, `curl`, `sha256sum` | unpacking the MSIX and the NuGet metadata, downloads, checksums | any |
| `git` | cloning | any |
| Node.js | only for UIs such as the Sidra fork and for the [mock service](api.md#mock-service) in `test/` (`node test/mock-daemon.mjs`) | see the UI's own instructions |

Why these exact pieces:

- The app is x64 only, and the wrapper and shims are built as x86_64 Windows binaries with
  `winegcc`, so a 64-bit Wine is required. 32-bit support in Wine is not needed by the app.
- `winegcc -b x86_64-windows` does not compile by itself: it drives a PE cross compiler, `clang`
  with `lld` (`clang -target x86_64-windows -fuse-ld=lld`) or the mingw-w64 GCC. Without one it
  fails with `winegcc: Could not find gcc`.
- Audio goes through Wine's `winepulse` driver. `bin/amd` preloads a small Linux library
  (`wrapper/build/libampulse.so`) into the Wine session that hooks libpulse to give the stream a
  readable name; with another Wine audio driver that part does nothing and playback is untested.
- The app's TLS goes through Wine's `secur32`/schannel, which uses GnuTLS. Without GnuTLS support
  in your Wine build, nothing can connect.
- `widl` compiles the proxy/stub DLL in `shim/ps/` and needs Wine's IDL files
  (`include/wine/windows/inspectable.idl` and friends). `shim/ps/build.sh` looks next to `widl`
  first, then in the usual distribution and WineHQ locations; `WINE_INCLUDE_DIR=<dir>` overrides
  the search.

`setup-prefix.sh` checks the tools it needs (Wine and its build tools, a test compile with
`winegcc` for the PE cross compiler, winetricks, `unzip`, `curl`, `sha256sum`, the C compiler and
`make`, the libpulse headers, PulseAudio or PipeWire-pulse, and a Python that can import
`dnfile`) before it changes anything, and lists what is missing. To run only this check, after
cloning (below):

```sh
./setup-prefix.sh --check
```

It exits with 0 and `all requirements found`, or with an error that lists what is missing. It
does not check that your Wine is 64-bit or has GnuTLS and `winepulse`.

### Arch Linux and derivatives

Arch's `wine` package follows the Wine development releases and contains `winegcc`, `widl` and
the headers.

```sh
sudo pacman -S --needed wine winetricks libpulse gnutls base-devel clang lld unzip curl git python
```

(`mingw-w64-gcc` instead of `clang lld` works too.)

### Fedora

Fedora's own `wine` package may lag behind the development releases. The WineHQ packages are the
tested route; `wine-devel-devel` contains `winegcc`, `widl` and the headers. Replace `43` with
your Fedora release.

```sh
sudo dnf config-manager addrepo --from-repofile=https://dl.winehq.org/wine-builds/fedora/43/winehq.repo
sudo dnf install winehq-devel wine-devel-devel
sudo dnf install winetricks pulseaudio-libs-devel gcc make clang lld unzip curl git python3
```

### Debian and Ubuntu

The distribution packages of Wine are too old to have been tested; use the WineHQ packages
(development branch). Follow the repository setup on
[the WineHQ download page](https://gitlab.winehq.org/wine/wine/-/wikis/Download) for your release,
then:

```sh
sudo apt install --install-recommends winehq-devel wine-devel-dev
sudo apt install winetricks libpulse-dev build-essential clang lld unzip curl git python3 python3-venv
```

`winehq-devel` links `wine`, `winegcc`, `widl` and friends into `/usr/bin`; `wine-devel-dev`
holds the headers under `/opt/wine-devel/include`, where the build finds them through the `widl`
link. If your distribution's `winetricks` is old,
use the current release from [the winetricks project](https://github.com/Winetricks/winetricks).

### Python and dnfile

Most distributions do not package `dnfile`. A virtual environment in the checkout avoids touching
the system Python; the commands are in [Clone and build](#clone-and-build), right after cloning.

## The Apple Music package

The project needs the **Apple Music for Windows** package as a file: the x64 `.msix`, or the
`.Msixbundle` that contains it (the extension's case does not matter to the script). You have to obtain this file yourself, as your own copy of the
app. This project does not provide it and does not describe how to get it.

### Expected version and checksums

This release is built against **Apple Music for Windows 1.1792.39941.0**:

| File | SHA-256 |
|---|---|
| `MusicPackage_1.1792.39941.0_x64.msix` (x64 package) | `84499bf9b4d3548fb77607dff5eece6f8b8d1a40b62eda78fc0bfc4f03aa3be7` |
| the `.Msixbundle` of the same version | `8d6c4069666d446046cda42d32b1765cb08d96716e446080a7c5894a819b0477` |

```sh
sha256sum /path/to/MusicPackage_1.1792.39941.0_x64.msix     # or the path of your .Msixbundle
```

`setup-prefix.sh` checks this itself and refuses any other file. The reason: the wrapper calls
into the app's interfaces by vtable slot and interface ID, typed in from this exact version; a
different build can move them and fail in ways that look like random crashes.

If you have a newer version, you can try it at your own risk with
`AM_ALLOW_UNTESTED_MSIX=1 ./setup-prefix.sh <file>`. The script then accepts a package whose
checksum or version differs (it still has to be an x64 Apple Music package) and prints a warning.
Expect breakage; if it works, please report the version.

## Clone and build

```sh
git clone https://github.com/Terrails/wine-engine-for-apple-music.git ~/src/wine-engine-for-apple-music
cd ~/src/wine-engine-for-apple-music
```

Then create the Python environment in the checkout:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
export AM_PYTHON="$PWD/.venv/bin/python"     # put this in your shell profile as an absolute path
```

Any Python 3 that can `import dnfile, pefile` works; without `AM_PYTHON` the scripts use
`python3`. `.venv/` is ignored by git. `./setup-prefix.sh --check` now tells you whether anything
is still missing.

`setup-prefix.sh` builds everything it needs, so you can go straight to the next step. To check
your toolchain first, without touching any Wine prefix:

```sh
env -u AM_PREFIX make -C wrapper           # wrapper/build/amd.exe and libampulse.so
env -u AM_PREFIX shim/build.sh             # shim/amshim.dll; fetches the metadata first
env -u AM_PREFIX shim/ampkg/build.sh       # shim/ampkg/ampkg.dll
env -u AM_PREFIX shim/ps/build.sh          # shim/ps/amps.dll (needs widl and Wine's IDL files)
env -u AM_PREFIX shim/build-stubs.sh       # amcertreq.dll, webauthn.dll, bcp47mrm.dll
```

`shim/build.sh` and `shim/ps/build.sh` install into a prefix only when `AM_PREFIX` is set in the
environment, hence the `env -u`. `shim/build.sh` runs `tools/fetch-metadata.sh`, which
downloads three official Microsoft packages from nuget.org at pinned versions, checks their
SHA-256 and extracts only the WinRT metadata (`.winmd`) into `~/.cache/wine-engine-for-apple-music/metadata`:

| Package | Version | Used for |
|---|---|---|
| `Microsoft.Windows.SDK.Contracts` | 10.0.26100.7175 | Windows Runtime type definitions |
| `Microsoft.WindowsAppSDK.WinUI` | 2.3.9 | `Microsoft.UI.Xaml.winmd` |
| `Microsoft.WindowsAppSDK.InteractiveExperiences` | 2.1.9 | `Microsoft.UI.winmd`, `Microsoft.Foundation.winmd` |

The code generator for `amshim.dll` reads the type definitions from these files. These packages
require accepting Microsoft's license terms, as when they are added to a project with NuGet. The
script prints the links and downloads at once, without asking: **by running it, or
`setup-prefix.sh`, which runs it, you accept them**, so read them first:

- `Microsoft.Windows.SDK.Contracts`: <https://aka.ms/WinSDKLicenseURL>
- `Microsoft.WindowsAppSDK.WinUI` and `Microsoft.WindowsAppSDK.InteractiveExperiences`: the
  `license.txt` inside each package (on its nuget.org page, for example
  <https://www.nuget.org/packages/Microsoft.WindowsAppSDK.WinUI/2.3.9>); the script keeps a copy
  in `~/.cache/wine-engine-for-apple-music/metadata/licenses/`.

Nothing from the packages is part of this repository; the metadata stays in your cache.

## Set up the prefix

```sh
./setup-prefix.sh /path/to/MusicPackage_1.1792.39941.0_x64.msix    # or your .Msixbundle
```

The prefix is created at `~/.local/share/wine-engine-for-apple-music/prefix` (change it with
`AM_PREFIX`, see [configuration.md](configuration.md)). `WINEPREFIX` is ignored on purpose: this
never touches your default Wine prefix. Don't point `AM_PREFIX` at a prefix you use for other
programs: the scripts stop everything running in it when they start and stop the service.

What the script does, in order:

1. Checks the tools it needs and refuses to run while the service or the engine is running.
2. Verifies the package (version and SHA-256, see above), takes the x64 `.msix` out of a bundle
   and unpacks it to `~/.cache/wine-engine-for-apple-music/msix/<version>`.
3. Downloads the Apple Root CA certificate from `apple.com` and checks its SHA-256. Apple's
   sign-in server uses it and Linux CA bundles do not contain it; without it sign-in fails before
   anything is sent.
4. Downloads the Windows SDK and Windows App SDK metadata from nuget.org
   (`tools/fetch-metadata.sh`, see [Clone and build](#clone-and-build)); **by running setup you
   accept their license terms**. Then builds the wrapper, all shims and a small helper that adds
   a certificate to the prefix. Nothing up to here runs Wine, so a download or build error stops
   the script before the prefix is touched.
5. Creates the prefix (`wineboot`) with Wine's desktop integration (`winemenubuilder`) disabled,
   for the setup run and in the prefix itself: Wine adds no menu entries, `wine-extension-*`
   desktop files or file associations for this prefix to your desktop. The app is not meant to be
   started from a menu, and its file types should not open in it. Then it runs
   `winetricks -q vcrun2022 dxvk`, then `winetricks -q sandbox`: the Microsoft Visual C++
   2015-2022 runtime the app needs, DXVK, and `sandbox`, which unlinks the prefix's
   Documents/Music folders from your home directory and removes the `Z:` drive, so the app's
   library and everything else it writes stays inside the prefix.
6. Turns Wine's crash dialog off and disables Wine's Bluetooth driver `winebth.sys`, deleting the
   devices it recorded (it records every Bluetooth device the host sees in the registry, and the
   growing registry makes playback stutter; see [KNOWN_ISSUES.md](../KNOWN_ISSUES.md)).
7. Copies the app to `C:\Program Files\AppleMusic` in the prefix and adds `ampkg.dll` to the
   imports of the prefix's copy of `AMPLibraryAgent.exe`.
8. Installs the shims built in step 4 (`shim/install.sh`): `ampkg.dll`, `amshim.dll` with its
   WinRT class registrations, `amps.dll`, `amcertreq.dll`, `webauthn.dll`, `bcp47mrm.dll`.
9. Registers the engine's COM classes, generated from the package's own `AppxManifest.xml`
   (`tools/genagentreg.py`), with its proxy DLL's import
   `NdrStubCall3` renamed to `NdrStubCall2` (Wine has only the latter).
10. Adds the Apple Root CA to the prefix's trusted roots, ends the prefix's Wine session and
    prints the next steps.

The script is idempotent: running it again on an existing prefix refreshes the app files and the
shims and keeps the sign-in and the library. Downloads and the unpacked package are cached and
reused. If a step fails, the script ends with `error: setup-prefix.sh failed (exit <n>) at:
<step>`; fix the cause and run it again.

## Sign in

```sh
bin/amd-signin
```

This starts the engine without the service and opens **native prompts** (plain Windows dialogs
drawn by Wine) on your desktop: first your Apple Account and password, then the two-factor
code sent to your trusted devices. Afterwards it prints `exit=0 (0 = signed in)` and exits with
that code (`124` when it timed out); it gives up after 15 minutes (`bin/amd-signin <seconds>` changes that). You need to do
this once; the session persists in the prefix until you sign out. The service must not be running
(`bin/amctl quit`). The log goes to `~/.local/state/wine-engine-for-apple-music/logs/signin.log`
and stays on your machine.

**The first-run privacy notice.** On its first start the app shows Apple's welcome and privacy
notice and does not start its library and playback services until it has been accepted. There is
no UI here to show it in, so `bin/amd-signin` and `bin/amd` start the wrapper with
`AM_ACCEPT_WELCOME=1`, which **presses the notice's accept button on your behalf**. By running
these scripts you accept that notice, as you would by clicking "Continue" in the app on Windows.
Read Apple's privacy policy at <https://www.apple.com/legal/privacy/> if you have not. The
acceptance is stored in the prefix, so later starts go straight on.

If sign-in shows "There was an error connecting to the Apple Account server", that is a network
failure on the way to Apple (often transient): run it again. See
[troubleshooting.md](troubleshooting.md) for how to find the cause.

## First play

**This makes sound** on your default audio output. Turn the volume down first.

Start the service in one terminal; it stays in the foreground until it is told to quit:

```sh
bin/amd                      # port 7654 by default
```

The first start after setup takes a few seconds longer. The log goes to
`~/.local/state/wine-engine-for-apple-music/logs/amd.log` (the previous run's to `amd.prev.log`).

In a second terminal, wait until the service reports `"ready":true` and `"signedIn":true`. If you
set `AM_PREFIX` or `AM_PORT`, set the same values in every terminal: `bin/amctl` reads the API
token from the prefix and connects to the port.

```sh
bin/amctl status
```

Playing needs a catalogue **song ID** and the ID of the **album** it is on. The public iTunes
Search API (no account needed) gives both: `trackId` is the song, `collectionId` the album.

```sh
curl -s 'https://itunes.apple.com/search?term=goldberg+variations&entity=song&country=us&limit=5' |
  python3 -c 'import json,sys
for r in json.load(sys.stdin)["results"]:
    print(r["trackId"], r["collectionId"], r["artistName"], "-", r["trackName"])'
```

Use the `country` of your Apple Music account (`us`, `gb`, `fr`, `jp`, ...); IDs are the same in
most countries, but availability is not. Then:

```sh
bin/amctl volume 0.2
bin/amctl play <trackId> <collectionId>          # the album, starting at that song
bin/amctl status                                 # nowPlaying, position, format
bin/amctl pause
bin/amctl resume
bin/amctl quality lossless                       # aac | lossless | hires; applies to the next track
bin/amctl play-album <collectionId>
bin/amctl stop
bin/amctl quit                                   # ends the service, the engine and the prefix's Wine session
```

A trailing country code selects the storefront for that request (`bin/amctl play <song> <album>
gb`); the default is `us` or `AM_STOREFRONT`. All commands: `bin/amctl` without arguments.
The API behind it: [api.md](api.md).

## Putting the commands on your PATH

The scripts find the checkout through symlinks, so you can link them into a directory on your
PATH:

```sh
cd ~/src/wine-engine-for-apple-music
mkdir -p ~/.local/bin
ln -sf "$PWD/bin/amd" "$PWD/bin/amctl" "$PWD/bin/amd-signin" ~/.local/bin/
```

Do not link `bin/amd-env.sh`; it is sourced by the others, not run.

## Connecting a UI

The app's own window cannot run under Wine, so you use a separate UI that talks to the service.

- **Sidra fork (`wine-engine` branch).** [Sidra](https://github.com/wimpysworld/sidra) is a
  desktop client around music.apple.com. The
  [`wine-engine` branch of the fork](https://github.com/Terrails/sidra/tree/wine-engine) sends playback of catalogue
  songs to this engine instead of the browser, while browsing, search, library, playlists and
  lyrics come from the web page. It starts `bin/amd` itself when the service is not running. Set
  `AM_AGENT_DIR` to this checkout (or put `bin/amd` on your PATH, above) and follow
  `README-agent.md` in that branch. In short: `npm ci` in the Sidra checkout, then
  `scripts/run-with-agent.sh` (or `npm run start:agent`). It installs a desktop entry
  "Sidra (Wine engine)" (ID `sidra-wine-engine`).
- **Your own.** Anything that can send HTTP requests: read the token from the prefix and call the
  API, see [api.md](api.md). `bin/amctl` is a short shell script that does exactly that.

Only one player at a time is supported: whatever UI sends commands controls the one engine.

## Updating

```sh
bin/amctl quit                     # stop the service first
cd ~/src/wine-engine-for-apple-music
git pull
./setup-prefix.sh <the same .msix or .Msixbundle>
```

`bin/amd` rebuilds the wrapper when its sources changed, but the shims are only installed by
`setup-prefix.sh` (or `shim/install.sh`), so run setup after every update. Your sign-in, library and settings stay in the prefix. Read
[CHANGELOG.md](../CHANGELOG.md) for changes that need more.

A new version of the Apple Music app needs a release of this project that supports it (or
`AM_ALLOW_UNTESTED_MSIX=1`, above).

## Uninstalling

Stop the service (`bin/amctl quit`), then delete what the project created. With the default
paths:

| Path | What |
|---|---|
| `~/.local/share/wine-engine-for-apple-music/prefix` | the Wine prefix: the app, the shims, your sign-in session, the engine's library database, settings and the push certificate |
| `~/.cache/wine-engine-for-apple-music` | NuGet metadata, the unpacked package, the Apple Root CA download |
| `~/.local/state/wine-engine-for-apple-music` | logs |
| `~/.local/bin/amd`, `~/.local/bin/amctl`, `~/.local/bin/amd-signin` | symlinks, if you made them |
| the cloned repository | sources, the Python environment `.venv/` and the build outputs (`build/`, `wrapper/build/`, `shim/gen/`, the built DLLs) |

```sh
rm -rf ~/.local/share/wine-engine-for-apple-music ~/.cache/wine-engine-for-apple-music \
       ~/.local/state/wine-engine-for-apple-music
rm -f ~/.local/bin/amd ~/.local/bin/amctl ~/.local/bin/amd-signin
```

If you set `AM_PREFIX`, `AM_CACHE_DIR`, `AM_LOG_DIR` or the `XDG_*_HOME` variables, delete those
locations instead.

Optionally, two caches that following this guide filled are not specific to this project:
`~/.cache/winetricks` (winetricks' downloads, here `vcrun2022` and DXVK; shared with your other
prefixes, delete it only if nothing else uses winetricks) and pip's download cache
(`~/.cache/pip`, from creating `.venv`).

A UI keeps its own files. For the Sidra fork with its defaults: the desktop entry
`~/.local/share/applications/sidra-wine-engine.desktop`, its icons
`~/.local/share/icons/hicolor/*/apps/sidra-wine-engine.png`, and its Electron profile
`~/.config/sidra-wine-engine` (`AM_WEBUI_PROFILE`, `AM_WEBUI_DESKTOP_ID` change them;
`README-agent.md` in the fork has the details).

```sh
rm -f ~/.local/share/applications/sidra-wine-engine.desktop \
      ~/.local/share/icons/hicolor/*/apps/sidra-wine-engine.png
rm -rf ~/.config/sidra-wine-engine
update-desktop-database -q ~/.local/share/applications 2>/dev/null
```

Deleting the prefix does not sign this machine out on Apple's side. To check which devices are
associated with your account, see the device list at <https://account.apple.com>.
