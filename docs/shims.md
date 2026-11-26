# Shims

The app is a packaged (MSIX) Windows application built on WinRT, the Windows App SDK and
Apple's own Windows ports of CoreFoundation, CFNetwork and friends. Wine runs most of it as is.
The DLLs in `shim/` fill the gaps that remain. `shim/install.sh` builds them and installs them
into `$AM_PREFIX`; `setup-prefix.sh` calls it ([install.md](install.md)).

| DLL | Source | Installed to | Loaded by |
|---|---|---|---|
| `ampkg.dll` | `shim/ampkg/` | `C:\Program Files\AppleMusic\` | the agent (first import), the wrapper (`LoadLibrary`) |
| `amshim.dll` | `shim/` (+ generated `shim/gen/`) | `C:\windows\system32\`, WinRT class registrations | any process activating one of its classes |
| `amps.dll` | `shim/ps/` | `system32`, `regsvr32` | COM, when the agent's interfaces are marshalled |
| `amcertreq.dll` | `shim/certreq/` | `system32`, `regsvr32` | crypt32, through its OID function registry |
| `webauthn.dll` | `shim/webauthn/` | `system32` | AuthKit (sign-in, in the wrapper process) |
| `bcp47mrm.dll` | `shim/bcp47mrm/` | `system32` | MRT Core (the app's resource loader) |

The guiding rule is to reuse what exists: official redistributables and community components
through winetricks, Wine's own DLLs (MSXML6, urlmon, WinHTTP, bcrypt, `PropertyValue`) behind
the shims, and the libraries the app ships (its `zlib1.dll`, MRT Core). A shim implements only
what nothing provides.

**Why shims and not Wine patches?** Some of what follows is specific to this app and has no
place in Wine (faking the identity of one particular package, answering one particular silent
audio stream). Other parts are places where Wine behaves differently from Windows and could be
fixed upstream; those are marked **upstream candidate** below. Shipping them as shims keeps the
project working on a stock distribution Wine today. Large parts of `amshim.dll` are generated
stubs that only satisfy the app's calls; they are not implementations Wine could take as they
are.

## ampkg.dll

The agent loads it as its first import (`tools/addimport.py` adds `ampkg.dll!ampkg_init` to the
prefix's copy of `AMPLibraryAgent.exe`); the wrapper loads it before anything else. It replaces
functions in every module of the process: it patches the import tables and delay-load import
tables of all loaded modules, and of modules loaded later (`LdrRegisterDllNotification`),
hot-patches the implementations in kernelbase so direct callers see the same, and hooks
`GetProcAddress`. `shim/ampkg/ampkg.h` has the overview; one file per topic:

- **`identity.c`: MSIX package identity.** A packaged app asks the OS who it is
  (`GetCurrentPackageFullName`, `GetCurrentPackageId`, `GetCurrentPackageInfo`,
  `GetCurrentApplicationUserModelId`, 29 functions in all). Under Wine nothing is installed as a
  package, so these fail with `APPMODEL_ERROR_NO_PACKAGE` and the app stops. ampkg answers them
  with the identity of the tested package (`AppleInc.AppleMusicWin_1.1792.39942.0_x64__nzyj5cx40ttqa`,
  application ID `App`); the package path is the executable's directory. *App-specific: the
  general fix would be MSIX deployment in Wine.*
- **`compat.c`: functions Wine lacks or gets wrong here.**
  - `CreateDispatcherQueueController` is redirected to amshim's implementation.
  - `SHGetKnownFolderPath`: Wine rejects the package-redirection flags
    (`KF_FLAG_NO_PACKAGE_REDIRECTION` and related) with `E_INVALIDARG` and a null path; ampkg
    drops them, since there is no package redirection. **Upstream candidate.**
  - `combase!RoTransformError` is unimplemented in Wine, and calling an unimplemented export
    aborts the process. WIL calls it on error paths, which turned recoverable errors into agent
    crashes. ampkg reports success (it only attaches error information). **Upstream candidate**
    (a trivial stub).
  - `dcomp!DCompositionCreateSurfaceHandle` is unimplemented (aborts). The agent calls it when
    it creates a player, for the video layer, even for audio. ampkg hands out a placeholder
    handle; video output cannot work without DirectComposition.
- **`audio.c`: IAudioClient lifetime.** Two differences from Windows that crashed the agent
  after a number of song switches ([architecture.md](architecture.md), "Design notes"):
  Wine keeps signalling the event handle passed to `SetEventHandle` after the app closed it
  (ampkg gives Wine a duplicate, closed with the client), and Wine's `IAudioSessionControl`
  keeps its audio client alive (ampkg makes that reference weak). The vtable is found through a
  hooked `CoCreateInstance(MMDeviceEnumerator)`. **Upstream candidates** (mmdevapi /
  winepulse: duplicate the event handle; session controls should not keep the client alive).
- **`audio_idle.c`: the idle stream.** The agent's silent start-up audio client is answered
  without a real stream (it is created on demand if real audio ever arrives);
  `AM_AUDIO_IDLE_STREAM=1` turns this off. *App-specific and cosmetic.*
- **`crypto.c`: the push service's TLS client certificate.** The push service
  (`ApplePushDirect.dll`) makes an RSA key pair, has a client certificate issued for it and
  connects to the push courier with it. Three Wine differences broke that, each fatally
  (`CFReadStreamSetProperty failed on kCFStreamPropertySSLSettings`):
  1. `CertSetCertificateContextProperty(CERT_KEY_PROV_HANDLE_PROP_ID)`: Windows takes the
     `HCRYPTPROV` itself as `pvData`; Wine's crypt32 dereferences it. ampkg passes it Wine's
     way. **Upstream candidate.**
  2. rsaenh writes a generated key pair to its registry storage only when the provider is
     released, but schannel reads the private key from there. Windows CSPs persist a new pair at
     once. ampkg's `CryptGenKey` generates an exportable pair, exports it and imports it back
     with the caller's flags (rsaenh stores imported pairs at once). **Upstream candidate**
     (rsaenh should persist on generation).
  3. The app creates the certificate context from a buffer with one or two bytes after the DER
     certificate. crypt32 keeps them, Wine's schannel hands the whole buffer to GnuTLS, which
     rejects it. ampkg trims `CertCreateCertificateContext`'s input to the DER length.
     **Upstream candidate** (crypt32 could store only the DER, as Windows tolerates the extra
     bytes).
- **`applog.c`** (optional, `AM_AGENT_OSLOG=1`): the process's own os_log messages on the Wine
  log as `applog[n]:` lines. **`handles.c`** (optional, `AM_HANDLE_DEBUG=1`): reports failing
  `CloseHandle` calls. Diagnostics, see [troubleshooting.md](troubleshooting.md).

## amcertreq.dll

Wine's crypt32 has no encoder for `X509_CERT_REQUEST_TO_BE_SIGNED`
(`CERT_REQUEST_INFO` → DER; "Unimplemented encoder for lpszStructType OID 4"). The push
service builds its certificate request with
`CryptSignAndEncodeCertificate(X509_CERT_REQUEST_TO_BE_SIGNED)`, which fails without it
(`Error return in provisioner: 0x2`), so push never connects.

amcertreq plugs the encoder in through crypt32's documented OID function registry
(`CryptDllEncodeObjectEx`, encoding type 1, OID `#4`); `regsvr32 amcertreq.dll` registers it
(done by `shim/install.sh`). It builds only the outer `CertificationRequestInfo` SEQUENCE; the
subject is already DER, the public key and attributes go through crypt32's own encoders. The
result verifies with `openssl req -verify`.

**Upstream candidate**: this belongs in crypt32's encoder table. It is a separate DLL only
because the registry mechanism makes that possible without patching Wine.

## amps.dll

A proxy/stub DLL generated by widl from `shim/ps/amps.idl` for `IInspectable`, `IAsyncInfo` and
`IAsyncOperation<HSTRING>` with its completion handler. The wrapper and the agent talk over COM
across processes; every asynchronous `IAMPLibrary` method returns an
`IAsyncOperation<HSTRING>`. Windows ships system proxy/stubs for these interfaces; the app's
`AMPLibraryAgent.Proxies.dll` does not contain them (its delegating stubs rely on the system's
`IInspectable` one), and Wine registers none. `shim/ps/build.sh` runs widl and links the DLL;
it is registered with `regsvr32`.

The app's proxy DLL also imports `rpcrt4!NdrStubCall3`, which Wine lacks; `setup-prefix.sh`
renames the import to `NdrStubCall2` in the prefix's copy (same signature, and the DLL uses DCE
format strings).

**Upstream candidates**: Wine could register proxy/stubs for these Windows interfaces, and
rpcrt4 could export `NdrStubCall3` for the DCE case.

## webauthn.dll and bcp47mrm.dll

Two small stubs for DLLs the tested Wine does not ship:

- `webauthn.dll`: the Windows WebAuthn API, as far as AuthKit (`AuthKitWin.dll`, sign-in)
  uses it: API version 1, no user-verifying platform authenticator, `GetAssertion` returns
  `ERROR_NOT_SUPPORTED`. Passkeys are not available; password plus two-factor code is.
- `bcp47mrm.dll`: `IsWellFormedTag`, the BCP-47 syntax check MRT Core
  (`Microsoft.Windows.ApplicationModel.Resources.dll`, bundled with the app) uses for language
  tags.

**Upstream candidates** as stubs (a real `webauthn.dll` would need a FIDO implementation).

## amshim.dll: WinRT classes

Wine implements only part of WinRT. AMP.Services and the agent use about 80 runtime classes
that Wine lacks or implements too thinly: `Windows.Web.Http`, `Windows.Data.Json`,
`Windows.Data.Xml.Dom`, `Windows.Storage`, `ApplicationData`, DispatcherQueue, resources,
globalization helpers, and a few Windows App SDK (`Microsoft.UI.*`) classes that AMP.Services
touches outside any XAML UI. amshim provides them, registered in the prefix under
`HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId` (which takes precedence over Wine's
registrations for the same names).

### The generator

Writing every method of 80 classes by hand is not practical, so most of the DLL is generated
from the Windows metadata:

- `shim/classes.txt` lists the runtime classes, one per line (`#` comments say why a class is
  there).
- `shim/build.sh` runs `tools/gen.py shim/`, which reads the `.winmd` metadata (Windows SDK
  contracts and Windows App SDK, downloaded by `tools/fetch-metadata.sh`; see
  [development.md](development.md)) and writes `shim/gen/amshim_gen.c` and `amshim_gen.h`:
  every interface of every listed class (instance, static and factory interfaces), the IIDs
  including parameterised ones, the vtables, the class tables, and async operation types.
  Then it compiles everything with winegcc. With `AM_PREFIX` set it also copies the DLL into
  the prefix and imports the class registrations.
- **Generated defaults**: properties use per-object storage (a `put_X` stores, `get_X` returns
  it); events hand out tokens; factory `Create*` methods store arguments whose names match
  properties; every other method logs `fixme:amshim:<class> <iface>::<method> stub` and returns
  `S_OK` with zeroed outputs, or `E_NOTIMPL` when it should return an object.
- **Overrides**: a plain C function named `OV_<InterfaceShortName>_<Method>` (overloads `_2`,
  `_3`, ...) in any `shim/*.c` file replaces the default for that method; it receives the
  object (`struct obj *`) and the method's parameters. `INIT_<Class>(struct obj *)` runs after
  construction; `DISPATCH_<Class>` can intercept all methods of a class. The generator finds
  these by regular expression, so they must be plain functions: names built with `##` macro
  pasting are never wired up, and `build.sh` refuses them.
- **Runtime**: `amshim_rt.c` (objects, interface tear-offs, weak references, factories,
  `DllGetActivationFactory`), `amshim_async.c` (`async_create`, `async_create_pending` +
  `async_complete`, `async_wait`), and synthetic helper classes listed in `gen.py`'s `SYNTH`
  table (string vector, map views, iterators, `ByteBuffer`, ...).

Tracing: `WINEDEBUG=+amcall` logs overridden methods with their result, `+amshim` the generated
ones.

### What is implemented

Areas split over several files have an `amshim_<area>.h` overview and an `amshim_<area>.c` with
shared helpers; the `OV_*`/`INIT_*` functions live in `ov_<area>_*.c`.

| Files | Area |
|---|---|
| `amshim_http.*`, `ov_http_*.c`, `ov_httpheaders.c`, `ov_smap.c` | `Windows.Web.Http` on WinHTTP: sets `Content-Length` on buffer and string content (as Windows does; AMP.Services asserts it); resends once when a kept-alive connection turns out closed (Windows does, Wine's WinHTTP fails with `ERROR_WINHTTP_INVALID_SERVER_RESPONSE`); inflates gzip bodies with the app's own `zlib1.dll`; cache freshness headers (`MaxAge`, `Expires`, `LastModified`); `AMSHIM_HTTP_DUMP=C:/dir` saves response bodies |
| `amshim_xml.*`, `ov_xml_*.c` | `Windows.Data.Xml.Dom` on MSXML6 (blanks `<!DOCTYPE>` before `loadXML`) |
| `amshim_json.*`, `ov_json_*.c` | `Windows.Data.Json` with its own parser and serializer (Wine's lacks parsing and most of the object model) |
| `amshim_storage.*`, `ov_storage_*.c` | `ApplicationData`, `StorageFolder`/`StorageFile` (basic properties, rename, move, copy), `FileIO` |
| `ov_collections.c`, `amshim_settings.c` | settings containers; the ApplicationData ones persist in the registry (below) |
| `ov_threading.c`, `ov_dispatcher.c` | `Windows.System.Threading`, DispatcherQueue for both `Windows.System` and `Microsoft.UI.Dispatching` (a message-only window per queue) |
| `ov_resources.c` | resource lookups served by the app's bundled MRT Core from `resources.pri`, opened by explicit path |
| `ov_package.c`, `ov_uri.c`, `ov_crypto.c`, `ov_urldecoder.c` | `Package`/`PackageId` (from ampkg's identity), `Uri` on urlmon, hashing on bcrypt, `WwwFormUrlDecoder` |
| `ov_datareader.c`, `ov_buffer.c`, `ov_vector.c` | streams and buffers |
| `ov_globalization.c` | `NumeralSystemTranslator`, `DecimalFormatter` (formatting only), `LoggingChannel` → `amlog[...]` lines |
| `ov_misc.c` | `ApiInformation`, `UserDataProtectionManager` (not available), `PackageManager` (empty), `CoreApplication`, `DateTimeFormatter`, `Deferral`, ... |
| `ov_winui.c` | the few WinUI types AMP.Services uses without a UI: `PropertyChangedEventArgs`, `Colors`, `ColorHelper.FromArgb`, `ThicknessHelper` |

**Upstream candidates** among these: the WinHTTP keep-alive resend (in Wine's WinHTTP itself),
`Windows.Data.Json` parsing, `Windows.Web.Http` as a whole, and the HTTP content
`Content-Length` behaviour would all be useful in Wine's own WinRT DLLs. They would have to be
rewritten in Wine's style first; the generated parts are not real implementations.

### Settings

`ApplicationData.LocalSettings` / `RoamingSettings` and their child containers
(`CreateContainer`) are registry keys under
`HKCU\Software\AppleMusicWrapper\Settings\{Local,Roaming}[\<container>...]`
(`amshim_settings.c`). One registry value per setting: the registry type is
`0x100 + PropertyType` (for example `hex(10b)` Boolean, `hex(105)` UInt32, `hex(10c)` String),
the data is the raw little-endian value (strings UTF-16 with a terminating NUL). Values go in
and out as Wine's `Windows.Foundation.PropertyValue` objects and are read on every lookup, so
all processes of the prefix (`amd.exe`, `lab.exe`, the agent) share them, as packaged apps'
settings are shared on Windows. Types not stored (arrays, Point/Size/Rect, composite values,
other objects) stay in memory for the process (`fixme:amshim:settings_write`).
`ApplicationData.ClearAsync` does nothing.

Keys seen in use: `GDPRWelcomeDialog_ShownContentVersion` (the first-run notice),
`whatsNewLevel`, `SignInPromptShown`, `DidMigrateFairPlayPrefKey`,
`WasAppleMusicSupportedWhenNetworkWasOnline`, and per-library containers with
`Sidebar_Library_*` keys. Resetting them: [troubleshooting.md](troubleshooting.md).

*App-specific location*: on Windows these live in the package's `settings.dat`; a Wine
implementation would need its own storage design.

## winetricks components

`setup-prefix.sh` installs these instead of reimplementing anything, right after creating the
prefix and before any other step (`winetricks -q vcrun2022 dxvk`, then `winetricks -q sandbox`),
so everything after them runs against the final layout:

- **`vcrun2022`**: the Microsoft Visual C++ 2015-2022 runtime. Most of the app's DLLs link
  against `msvcp140.dll`/`vcruntime140.dll`. On Windows they come from the VCLibs framework
  packages the MSIX declares as dependencies (`Microsoft.VCLibs.140.00` and its UWPDesktop
  variant), which are not inside the MSIX. winetricks installs Microsoft's own runtime instead
  of relying on Wine's builtin one.
- **`dxvk`**: Direct3D 11 / DXGI on Vulkan. The agent's media stack (`AVFoundationCF.dll`,
  `CoreVideo.dll`) uses Direct3D 11. DXVK is the implementation most Wine setups run; the
  project is tested with it, and audio playback has not been tested on Wine's own wined3d.
- **`sandbox`**: unlinks the prefix's user folders from your home directory and removes the
  `Z:` drive, so the agent's library and everything else the app writes stays in the prefix
  ([architecture.md](architecture.md), "The prefix is sandboxed").

Not installed: the WebView2 runtime. The engine never creates a WebView in this setup; sign-in
steps that would need one (server-provided web UI) are not supported.
