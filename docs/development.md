# Development

How to build the parts, how the code is laid out, how to add a WinRT class to the shim, and the
tools used to find out what the app expects. Read [architecture.md](architecture.md) first;
[troubleshooting.md](troubleshooting.md) has the log patterns that point at missing pieces.

Build-only commands (make, winegcc, widl, the generators) do not touch any Wine prefix. Every
command that runs Wine (`bin/amd`, `bin/amd-signin`, `setup-prefix.sh`, `shim/install.sh`,
`dev/*.sh`) works in `$AM_PREFIX` only. `bin/amd`, `bin/amd-signin`, `setup-prefix.sh` and
`dev/*.sh` also end that prefix's Wine session; check first that the service is not in use
(`bin/amctl status`). `bin/amctl` runs no Wine.

## Requirements

What you need to build and run is listed in [install.md](install.md). For development in
addition:

- Python 3 with the packages in `requirements.txt` (`dnfile`, which brings `pefile`), in a
  virtual environment if you like; point `AM_PYTHON` at its interpreter.
  `dev/gapmon.py` also needs `pip install -r dev/requirements.txt` (numpy) and `parec`.
- Node.js 18 or later for the mock service (`test/mock-daemon.mjs`).
- `llvm-objdump` (or any objdump with PE support) if you use `dev/disfn.py`.

## Building

| Command | Builds | Needs |
|---|---|---|
| `make -C wrapper` | `wrapper/build/amd.exe` (the service) and `wrapper/build/libampulse.so` (Linux, the stream name and mute) | winegcc, a C compiler, libpulse headers |
| `make -C wrapper lab` | `wrapper/build/lab.exe` from `dev/lab/lab.c` plus `wrapper/src` without `main.c` | as above; optionally the app's metadata (below) |
| `shim/build.sh` | `shim/amshim.dll` (generated + hand-written) | winegcc, Python with dnfile, the Windows metadata |
| `shim/ampkg/build.sh` | `shim/ampkg/ampkg.dll` | winegcc |
| `shim/ps/build.sh` | `shim/ps/amps.dll` (widl output in `shim/ps/`) | widl, winegcc, Wine's IDL files (found next to widl or in the usual package locations; `WINE_INCLUDE_DIR=<dir with inspectable.idl>` overrides) |
| `shim/build-stubs.sh` | `shim/webauthn/webauthn.dll`, `shim/bcp47mrm/bcp47mrm.dll`, `shim/certreq/amcertreq.dll` | winegcc |
| `shim/install.sh` | all shims, then installs them into `$AM_PREFIX` | everything above, a set-up prefix |

`setup-prefix.sh` also builds `build/addroot.exe` (from `tools/addroot.c`) for the certificate
step; it is not used otherwise.

`make` recompiles only what changed (dependency files in `wrapper/build/`). `bin/amd`,
`bin/amd-signin` and `dev/lab.sh` run make before they start, so the wrapper is always current;
the shims are installed only by `shim/install.sh` (or `setup-prefix.sh`).

`shim/build.sh` and `shim/ps/build.sh` install into the prefix **only when `AM_PREFIX` is set in
their environment** (`shim/install.sh` sets it). For a build-only check, unset it:

```sh
env -u AM_PREFIX -u WINEPREFIX make -C wrapper
env -u AM_PREFIX -u WINEPREFIX shim/build.sh
env -u AM_PREFIX -u WINEPREFIX shim/ampkg/build.sh
env -u AM_PREFIX -u WINEPREFIX shim/ps/build.sh
env -u AM_PREFIX -u WINEPREFIX shim/build-stubs.sh
```

### Metadata and what is generated from what

Nothing generated and nothing derived from Microsoft's or Apple's metadata is in the
repository; it is produced on your machine.

| Generated | From | By |
|---|---|---|
| `$AM_METADATA_DIR/sdk/*.winmd` | `Microsoft.Windows.SDK.Contracts` (nuget.org) | `tools/fetch-metadata.sh` |
| `$AM_METADATA_DIR/winui/Microsoft.UI.Xaml.winmd`, `Microsoft.UI.winmd`, `Microsoft.Foundation.winmd` | `Microsoft.WindowsAppSDK.WinUI`, `Microsoft.WindowsAppSDK.InteractiveExperiences` (nuget.org) | `tools/fetch-metadata.sh` |
| `shim/gen/amshim_gen.c`, `amshim_gen.h` | `shim/classes.txt`, the `OV_*`/`INIT_*`/`DISPATCH_*` functions in `shim/*.c`, the metadata above | `tools/gen.py` (run by `shim/build.sh`) |
| `shim/amshim.reg` | `shim/classes.txt` | `shim/build.sh` (install step only) |
| `shim/ps/amps.h`, `amps_i.c`, `amps_p.c`, `dlldata.c` | `shim/ps/amps.idl` | widl (`shim/ps/build.sh`) |
| `wrapper/build/iids_gen.h` | the app's own `AMP.*.winmd` and `Microsoft.UI.Xaml.winmd` | `tools/geniids.py` (run by `make lab`) |

- `tools/fetch-metadata.sh` downloads pinned package versions from nuget.org, checks each
  package's SHA-256 before extracting anything, keeps only the `.winmd` files and records what
  it fetched, so later runs return at once. `AM_METADATA_DIR` defaults to
  `$AM_CACHE_DIR/metadata`. The packages come with Microsoft's license terms, which the script
  prints; the metadata stays in your cache. `shim/build.sh` and `tools/q` run it when the
  metadata is missing.
- `make lab` looks for the app's own metadata in `WINMD_DIR`, else in the installed app
  (`$AM_PREFIX/drive_c/Program Files/AppleMusic`), else in the unpacked package under
  `$AM_CACHE_DIR/msix/<version>`. Without it, `lab.exe` is still built, with `probe_iids()` (used by
  `AM_EXPLORE`) disabled.
- The hand-written GUIDs and vtable slot numbers in `wrapper/src` and `shim/ampkg` are interop
  constants for the tested package version, found with the tools below. A different app
  version may move slots; check them with `tools/q` before trusting them.

## Code layout

| Path | What |
|---|---|
| `bin/` | `amd` (run the service), `amctl` (API client), `amd-signin`, `amd-env.sh` (sourced by all scripts: defaults, prefix, Wine session). The scripts resolve their own location, so `bin/` can be linked into `~/.local/bin` |
| `setup-prefix.sh` | creates and completes a prefix ([install.md](install.md)) |
| `wrapper/src/` | the service `amd.exe` (file table in [architecture.md](architecture.md)) |
| `wrapper/pulse/ampulse.c` | `libampulse.so` |
| `wrapper/Makefile` | builds into `wrapper/build/` |
| `shim/` | `amshim.dll`: runtime `amshim_rt.c`, `amshim_async.c`, area helpers `amshim_<area>.{c,h}`, overrides `ov_*.c`, `classes.txt`, `amshim.spec`, `build.sh`, `install.sh` |
| `shim/ampkg/` | `ampkg.dll`, one file per topic (overview in `ampkg.h`) |
| `shim/ps/`, `shim/certreq/`, `shim/webauthn/`, `shim/bcp47mrm/` | `amps.dll`, `amcertreq.dll`, the two stubs ([shims.md](shims.md)) |
| `tools/` | build-time tools: `gen.py` + `wmd.py` (metadata → C), `winmd.py` + `q` (metadata query), `fetch-metadata.sh`, `addimport.py` (adds an import to a PE file), `addroot.c` (adds a certificate to the prefix's Root store), `geniids.py`, `genagentreg.py` (the agent's COM registration from the package's `AppxManifest.xml`) |
| `dev/` | experiments and diagnostics: `lab/lab.c` + `lab.sh`, `run-agent.sh`, `dbg-agent.sh`, `disfn.py`, `gapmon.py` |
| `test/mock-daemon.mjs` | the mock service ([api.md](api.md)) |
| `docs/` | this documentation |

## Adding a WinRT class to amshim

When the log shows `Failed to find library for <Namespace.Class>` (or Wine's own implementation
of a class falls short):

1. Add the class's full name to `shim/classes.txt`, with a `#` comment saying why it is needed.
2. Generate: `env -u AM_PREFIX shim/build.sh` (or just the generator,
   `$AM_PYTHON -I tools/gen.py shim`). Every method now exists with a default body.
3. Look up the generated signatures in `shim/gen/amshim_gen.h` (search for the interface's
   short name) or with `tools/q <Namespace.Class>`.
4. Write overrides for the methods the app actually needs, in the matching `shim/ov_<area>.c`
   (or a new `ov_*.c`):

   ```c
   /* from shim/ov_uri.c: the Uri object keeps an urlmon IUri in o->priv */
   HRESULT OV_IUriRuntimeClass_get_Host(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_HOST, r); }
   HRESULT INIT_HttpClient(struct obj *o) { ... }     /* from shim/ov_http_client.c */
   ```

   `OV_<InterfaceShortName>_<Method>` (overloads get `_2`, `_3`, ... in metadata order) replaces
   the generated default; `HRESULT INIT_<Class>(struct obj *o)` runs after an object is
   constructed; the generator also supports `DISPATCH_<Class>`, which can intercept every
   method of a class (unused so far). The generator finds them by regular
   expression, so write them as plain functions (no `##` macro pasting; `build.sh` refuses it).
   Per-object state goes in `o->priv`; properties without an override already have storage.
5. Build, install (`shim/install.sh`, service stopped) and run. `WINEDEBUG=+amcall` traces your
   overrides with their results, `+amshim` the generated defaults; a `fixme:amshim:… stub` line
   is the next method to write.

Reuse Wine's own pieces where they exist (MSXML6 for XML, urlmon for URIs, WinHTTP, bcrypt,
`Windows.Foundation.PropertyValue`), and the libraries the app ships.

## Querying metadata: tools/q

`tools/q` prints a type from the metadata: its GUID, its attributes (activatable, static and
composable factories, exclusive-to), the interfaces a class implements, the fields of enums and
structs, and the methods (property accessors and event add/remove included) with their vtable
slots. For an interface the number in brackets is the vtable slot; for a class it is only the
member's position, so query the interface (`tools/q Windows.Foundation.IUriRuntimeClass`) for
slots.

```sh
tools/q Windows.Foundation.Uri                  # Windows SDK types (the fetched metadata)
tools/q --search DispatcherQueue
WINMD_DIR="$AM_PREFIX/drive_c/Program Files/AppleMusic" tools/q AMP.Services.IPlayer   # the app's own types
```

For the app's own interfaces (`AMP.*`), point `WINMD_DIR` at the installed app or at the
unpacked package in `$AM_CACHE_DIR/msix/<version>`; that metadata is part of your copy of the app and is
read in place.

**WinRT vtable slot rules.** Slots 0-2 are `IUnknown`, 3-5 `IInspectable`, so method *n* of an
interface (counting from 0 in metadata order, property getters and setters and event
add/remove included) is slot 6 + *n*. Common ones: `IAsyncInfo` `get_Status` 7, `get_ErrorCode`
8; `IAsyncOperation<T>::GetResults` 8; `IAsyncOperationWithProgress<T,P>::GetResults` 10. The
wrapper calls by slot with `VT`, `CALL0` and `CALL1` from `wrapper/src/common.h`, with the slot
and member name in a comment next to each call.

## Experiments: lab.exe

`dev/lab.sh [seconds=90] [WINEDEBUG=err,fixme,+ampkg,+amshim]` builds `lab.exe`
(`make -C wrapper lab`), copies it next to the app and runs it in the prefix; the log goes to
`$AM_LOG_DIR/lab.log`. `lab.exe` does the same start-up as `amd.exe` and then whatever the
environment asks for. The service must not be running.

| Variable | Effect |
|---|---|
| `AM_EXPLORE=1` | the current view model of the navigation stack and the interfaces it answers to (needs the generated IID table) |
| `AM_SEARCH=<query>` | what the sidebar search does (`Sidebar.SearchQuerySubmitted`); lists the resulting page |
| `AM_PLAY_TITLE=<text>` | with `AM_SEARCH`: the item to pick (title or class part; default `SquareLockup`) |
| `AM_PLAY=1` | with `AM_SEARCH`: play the picked item for 35 s (**makes sound**) |
| `AM_DEEPLINK=<url>` | open a deep link (`musics://music.apple.com/us/album/<albumId>`) and list the page |
| `AM_DUMP=1` | with `AM_DEEPLINK`: every item of the page with its play action's class |
| `AM_PLAY_ITEM=<s>:<i>` | with `AM_DEEPLINK`: run the play action of item `i` in section `s` (**makes sound**) |
| `AM_QUEUE=1` | with `AM_PLAY_ITEM`: dump the play queue |
| `AM_JUMP=<adamId>` | with `AM_QUEUE`: jump to that queue item |
| `AM_NOWPLAYING=1` | with `AM_PLAY_ITEM`: every field of the now-playing item |
| `AM_CD_TEST=a\|b\|…` | round-trip `ContentDescriptor` JSON candidates |
| `AM_CD_DUMP=1` | with `AM_DEEPLINK` or `AM_SEARCH`: serialise the first catalogue item's descriptor |
| `AM_CLOUD=1` | report the cloud library state; `AM_CLOUD_ON=1` also turns syncing on |
| `AM_SILENT=1` | engine volume 0 for the run, `AM_RESTORE_VOLUME` (default 1) afterwards |
| `AM_WAIT=<ms>` | how long to let the app settle first (default 8000) |
| `AM_POLL=<n>` | report the ServiceProvider life-cycle and agent state for *n* s at the end (default 5; above 5 also makes a read-only agent call each second) |

Example: `AM_SILENT=1 AM_DUMP=1 AM_DEEPLINK='musics://music.apple.com/us/album/<albumId>' dev/lab.sh 60`.
For silent tests a null sink is safer than `AM_SILENT` ([troubleshooting.md](troubleshooting.md)).
`lab.exe` keeps its own CFPreferences (domain `lab.exe`) but shares the ApplicationData settings
with `amd.exe`.

**IID table.** `tools/geniids.py <out.h> <dir or .winmd>...` writes every non-generic interface
IID of the app's metadata as a C table; `make lab` runs it into `wrapper/build/iids_gen.h` when
it finds the app (see "Metadata"). `probe_iids()` in `dev/lab/lab.c` then queries an opaque
object for each of them and logs which it answers: the quickest way to learn what an object
returned by the app is.

## Diagnostics in dev/

The shell scripts work in `$AM_PREFIX`, write to `$AM_LOG_DIR` and refuse to run while the
service does. `gapmon.py` needs the service running; `disfn.py` only reads files.

- **`dev/run-agent.sh <logname> [seconds=60] [WINEDEBUG]`**: runs `AMPLibraryAgent.exe -Embedding`
  alone, as COM would start it, with `err,fixme,warn+seh,+amshim,+ampkg` by default; log
  `$AM_LOG_DIR/<logname>.log`. Exit code 124 (still running at the timeout) is the good case.
  `AMSHIM_HTTP_DUMP=C:/some/dir` saves HTTP response bodies (a path inside the prefix; they
  contain account data, delete them afterwards).
- **`dev/dbg-agent.sh <logname> [exe=AMPLibraryAgent.exe]`**: runs the agent (or another of the
  app's executables) under winedbg and prints a backtrace at the first unhandled exception;
  `DBG=<WINEDEBUG>`, `T=<seconds>`. A crash without a GUI otherwise loses winedbg's output.
- **`dev/disfn.py <pe file> <listing.asm> <start va hex> [max lines] [--all]`**: a compact
  disassembly of one function from an `llvm-objdump -d` listing of the same file, with imports
  and strings resolved. Without `--all` only calls, returns and lines that reference data are
  shown. Use it to read what the app does around an address from a backtrace.
- **`dev/gapmon.py <seconds>`** (run with `python3 -I`): watches the engine's audio. It logs
  `SILENCE` (digital silence of 5 ms or more inside music on the default sink's monitor, with
  the track position from the service), `GAP` (silence on the engine's own stream) and stream
  events (appearing, corking). A `SILENCE` without a `GAP` at the same moment is a dropout. It
  only reads; note that attaching a monitor to the stream causes one click itself. Run it with
  playback routed to a null sink for unattended measurements.

## The mock service

`node test/mock-daemon.mjs [port=7699]` serves the API without Wine, with the token
`mocktoken`; details in [api.md](api.md). The Sidra fork's smoke test drives its UI against
it, silent and with hidden windows (it still needs a Wayland or X11 session).

## Editor setup (clangd)

`wrapper/src/compile_flags.txt`, `shim/compile_flags.txt` and `shim/ampkg/compile_flags.txt` tell
clangd to parse the sources as x86_64 Windows code with Wine's headers. clangd still reports
spurious errors in some `ov_*.c` files (generated declarations, Wine-specific attributes);
trust winegcc's output.

## Lessons

- **Argument evaluation order is unspecified in C.** A call like `bstr_out(get_x(&b), b, …)`
  passed `b` before `get_x` had set it. Fetch into a variable first, then call.
- **Never name a tool after a Python standard library module.** A `dis.py` next to the
  generator was imported instead of the standard `dis` and broke `gen.py`.
- **`pkill -f` / `pgrep -f` patterns match the calling shell too**, whose command line contains
  the pattern. Use a bracket pattern: `pgrep -f 'AMPLibraryAgen[t]\.exe'`. For processes in the
  prefix `ps -eo pid=,comm=` is often clearer: Wine processes show their Windows command line
  (`C:\Program Files\AppleMusic\AMPLibraryAgent.exe -Embedding`) in `ps -f` and `pgrep -f`.
- **Do not trust Wine's lifetime rules to match Windows.** All three song-switch crashes came
  from objects that Windows keeps alive (or lets go of) differently ([architecture.md](architecture.md),
  "Design notes").

## Continuous integration

`.github/workflows/build.yml` runs on pushes and pull requests and is **build-only**. On an
Ubuntu runner it installs the WineHQ development branch (`winehq-devel`, `wine-devel-dev` for
winegcc, widl and the IDL files), clang, the libpulse headers and shellcheck, then:

- checks the shell scripts (`sh -n` and `shellcheck -S warning`) and the Python tools
  (`py_compile`), and the mock with `node --check`;
- builds the wrapper (`make -C wrapper`, and `make -C wrapper lab` without app metadata);
- builds `amshim.dll` with the metadata fetched from nuget.org by `tools/fetch-metadata.sh`
  (cached between runs, keyed on the script, so a change of the pins fetches again), then
  `ampkg.dll`, `amps.dll`, the stubs and `addroot.exe`.

All build steps run with `env -u AM_PREFIX -u WINEPREFIX`. Nothing in it runs Wine, the app or any
setup or install step.

The engine cannot be tested in CI: it needs the app's MSIX package (proprietary; you download
your own copy, and it must not be redistributed), a signed-in account, network access to
Apple's servers and an audio server. End-to-end checks are done by hand, against a real prefix
with playback routed to a null sink ([troubleshooting.md](troubleshooting.md)). UI behaviour is
tested against the mock service.
