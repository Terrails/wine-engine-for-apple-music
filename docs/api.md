# Local API

The service (`amd.exe serve`, started by `bin/amd`) is controlled through a small HTTP API. This
page is for UI authors: transport, authentication, every endpoint as implemented in
`wrapper/src/api.c` and `wrapper/src/server.c`, the status fields, timing, and a mock service to
develop against without Wine.

`bin/amctl` is a ready-made shell client for every endpoint (`bin/amctl status`,
`bin/amctl play <songId> <albumId>`, ...).

## Transport

- HTTP/1.1, JSON responses, listening on **127.0.0.1 only**.
- Port: **7654** by default. `AM_PORT` selects another one for the service and the clients
  (see [configuration.md](configuration.md)); `bin/amd <port>` also takes it as an argument.
- One request per connection: every response carries `Connection: close`,
  `Cache-Control: no-store` and `Content-Type: application/json; charset=utf-8`.
- Parameters are passed in the **query string**; request bodies are ignored. Values are not
  URL-decoded, which is fine for the IDs, numbers and codes the API takes.
- The request line and headers must fit in 4 KB and arrive with no more than 5 s between two
  packets (a per-read timeout, not a total deadline).

## Authentication

Every request needs `Authorization: Bearer <token>`.

- The token is 64 hex characters, **random on every start** of the service, and written (no
  trailing newline) to `%LOCALAPPDATA%\AppleMusicWrapper\token` inside the prefix, i.e.
  `$AM_PREFIX/drive_c/users/<your user name>/AppData/Local/AppleMusicWrapper/token` on the
  Linux side.
- A wrong or missing token gives `401 {"error":"missing or wrong token"}`. Because the token
  changes when the service restarts, **re-read the file on a 401** and retry once.
- The header name must be written `Authorization` or `authorization`, followed by exactly one
  space after the colon, and the scheme exactly `Bearer` (`Authorization: Bearer <token>`).
- The file has your user's normal file permissions: any program running as you can read it
  and control playback. That is the intended trust boundary.

### No CORS, on purpose

The server sends no CORS headers. A web page in a browser therefore cannot drive the service
cross-site: it cannot read the token, and the custom `Authorization` header forces a CORS
preflight (`OPTIONS` without a token), which is refused with 401. Programs that are not
browsers (Node, Electron's main process, curl, Python) are not subject to CORS. A UI that runs
in a browser engine should make its requests from a privileged context (for example Electron's
main process) and never hand the token to the page.

## Endpoints

`GET` is accepted for `/status` only; everything else is `POST`.

| Endpoint | Parameters | Success response |
|---|---|---|
| `GET /status` | none | the status object (below) |
| `POST /play` | `album=<id>` (required), `song=<id>`, `storefront=<cc>`, `queue=single` | `{"ok":true,"hresult":"00000000","loadMs":812}` |
| `POST /pause` | none | `{"ok":true,"hresult":"00000000"}` |
| `POST /resume` | none | same |
| `POST /stop` | none | same |
| `POST /next` | none | same |
| `POST /previous` | none | same |
| `POST /seek` | `ms=<position>` | same |
| `POST /volume` | `v=<0..1>` | same |
| `POST /quality` | `q=aac\|lossless\|hires` | `{"ok":true,"hresult":"00000000","quality":"lossless"}` |
| `POST /quit` | none | `{"ok":true}`, then the service exits |

Details:

- **`/play`**: `album` and `song` are numeric catalogue IDs.
  - `song` + `album`: plays the album starting at the song (the queue continues with the
    album). With `queue=single`: the song alone, then the app's autoplay.
  - `album` alone: the whole album from the first track (`queue` is ignored).
  - `storefront`: the two-letter country code of the catalogue, lowercase (`us`, `gb`, `jp`,
    ...). Default: the service's `AM_STOREFRONT`, else `us`. A value that is not all
    lowercase letters is replaced by `us`.
  - `loadMs`: milliseconds until playback was requested.
  - The response comes when playback has been *requested*, not when sound starts; see
    "Timing".
- **`/pause`, `/resume`, `/stop`, `/next`, `/previous`** call the player directly. `ok` and
  `hresult` say whether the engine accepted the call, not that its effect is complete; read
  `/status` to see the result.
- **`/seek`**: `ms` is the position in milliseconds from the start of the track. A missing
  value seeks to 0.
- **`/volume`**: `v` is clamped to 0..1. A missing value sets 0.
- **`/quality`**: the streaming quality setting. It applies to tracks started afterwards and
  survives restarts (the agent keeps it in its own preferences). `quality` in the response is
  the value read back.
- **`/quit`**: answers first, then stops playback (about 1.5 s) and ends the service.
  `bin/amd` then ends the prefix's Wine session, including the agent.

### Errors

| Status | Body | When |
|---|---|---|
| 400 | `{"error":"bad request"}` | malformed request line |
| 400 | `{"ok":false,"error":"need album=<id> (and song=<id> for a single song), numeric"}` | `/play` without a numeric `album`, or a non-numeric `song`, or before the main page has loaded |
| 400 | `{"ok":false,"error":"need q=aac\|lossless\|hires"}` | `/quality` with another value, or before the playback settings exist |
| 401 | `{"error":"missing or wrong token"}` | see "Authentication" |
| 404 | `{"ok":false,"error":"item not found on the album page","pageError":false,"loadMs":10012}` | `/play`: the album page did not contain the song (or the album) within about 10 s; `pageError` is the page's own error flag |
| 404 | `{"error":"unknown endpoint"}` | any other path (a POST, once the player is ready; a GET gets 405, a POST before then 503) |
| 405 | `{"error":"use POST (GET only for /status)"}` | a method other than POST (except `GET /status`) |
| 500 | `{"ok":false,"hresult":"8000ffff"}` (plus `loadMs` for `/play`, `quality` for `/quality`) | the engine returned a failure HRESULT |
| 503 | `{"error":"player not ready"}` | any POST except `/quit` before the engine has a player |
| 503 | `{"error":"timed out waiting for the player thread"}` | the request was not handled within 60 s |

## Status

`GET /status` returns:

```json
{
  "ready": true,
  "signedIn": true,
  "playing": true,
  "paused": false,
  "volume": 0.800,
  "quality": "lossless",
  "nowPlaying": {
    "title": "Song title",
    "songId": "1234567890",
    "genre": "Electronic",
    "releaseDate": "2001-03-07T12:00:00Z",
    "artist": "Artist name",
    "album": "Album name",
    "format": "lossless",
    "sampleRate": "44.1",
    "bitDepth": "",
    "positionMs": 73512,
    "durationMs": 320357
  },
  "agentHung": false,
  "agentPhase": null
}
```

| Field | Type | Meaning |
|---|---|---|
| `ready` | boolean | the engine has a player object; POST commands work |
| `signedIn` | boolean | an account is signed in |
| `playing` | boolean | the player is playing |
| `paused` | boolean | the player is paused (`playing` and `paused` are both false when stopped or loading) |
| `volume` | number 0..1 | the engine's volume. With nothing playing it can be stale; read it while a track plays |
| `quality` | string | the **setting**: `aac`, `lossless`, `hires`, or `unknown` before the settings exist |
| `nowPlaying` | object or `null` | the current item; `null` when nothing is loaded, while the item is a placeholder, or while it has no title yet (the first moments of loading) |
| `agentHung` | boolean | an agent call has not returned within 5 s (playback and library calls are stuck) |
| `agentPhase` | string or `null` | while hung: the call that is stuck, e.g. `MediaAppExecAgentCommandAsync(5045)`; `null` otherwise |

`nowPlaying` fields:

| Field | Type | Meaning |
|---|---|---|
| `title` | string | track name, in the app's localisation for the storefront and the system language |
| `songId` | string | catalogue ID of the track (`ContentDescriptor.AdamId`), `""` if it has none. Use it to match the track you asked for and to look up artwork |
| `artist`, `album` | string | from the item's subtitle `<artist> — <album>`; if the subtitle has no ` — `, all of it is in `artist` and `album` is `""` |
| `genre` | string | primary genre |
| `releaseDate` | string | ISO 8601, may be `""` |
| `format` | string | what is **decoded now**: `other` (= AAC), `lossless`, `hires`, `atmos`, `spatial`, `surround` |
| `sampleRate` | string | in kHz as the app reports it, e.g. `"44.1"`; `"0"` in the first seconds |
| `bitDepth` | string | e.g. `"24"` for hi-res; often `""` for 16-bit and 48 kHz lossless |
| `positionMs`, `durationMs` | integer | position and duration in milliseconds |

Text is UTF-8; names are the app's localised ones and can differ from what the public iTunes
Search API returns for the same ID. Artwork is not in the status (the app has only an internal
`amp-artwork-id://` URI): look it up by `songId`, for example with the iTunes Search API
`lookup?id=<songId>`.

**`quality` vs `format`.** `quality` is what you asked for; `format` is what the engine is
playing. A track always starts on its AAC variant and switches within seconds, so a lossless
setting shows `format: "other"` and `sampleRate: "0"` for the first 1-4 s, then `lossless`. A
track that is not available in the requested quality stays on a lower format.

**Agent watchdog.** AMP.Services arms a 5 s timer around every call to the agent. If it fires,
`agentHung` becomes true and `agentPhase` names the call; when the call returns, `agentHung`
goes back to false. A UI should show something like "player engine not responding" meanwhile.
If the agent dies, the service exits (code 4) and a UI that started it should start it again.

## Timing

- **`/play` takes about 1-3 s** until it responds: the album page loads by deep link
  (typically 0.4-0.8 s), then the play action runs and, for a song in its album, the queue
  jump follows 0.3-0.6 s later. About one play in a dozen needs a second navigation and takes
  3.5 s or more. The worst case is about 15 s (up to 10 s for the page, up to 5 s for the
  album to reach the queue). Use a client timeout of more than 60 s (the server's own limit)
  for `/play`.
- Sound starts shortly after the response. Poll `/status` until `nowPlaying.songId` is the song
  you asked for and `playing` is true; until then the status may still show the previous track
  or `null`.
- `format` reads `other` for the first seconds of every track (see above).
- Polling `/status` every 250-500 ms is fine; the status path does not leak.
- Without gapless playback, a UI that advances its own queue (one `/play` per item) has a gap of
  the deep-link load time between tracks.

## Concurrency

The engine may only be called from its UI thread. A socket thread accepts **one connection at a
time** and hands the request to the main thread, which runs it from its message loop. So
requests are strictly serialised: while a `/play` is running, a `/status` from another client
waits until it is done (connections queue in the listen backlog). Keep requests short, do not
hold connections open, and do not poll from several timers in one client.

There is one player. Several clients may read `/status`, but only one should control playback
(see [architecture.md](architecture.md), "One player at a time").

## Mock service

`test/mock-daemon.mjs` is a silent stand-in with the same API: a clock instead of audio, no
Wine. Use it to develop and test a UI.

```sh
node test/mock-daemon.mjs 7699            # port, default 7699
curl -s -H 'Authorization: Bearer mocktoken' http://127.0.0.1:7699/status
```

- The token is always **`mocktoken`**.
- `/play` waits 0.9 s, looks the song up in the public iTunes Search API (network access
  needed; `storefront` default `AM_STOREFRONT`, else `us`) and starts it 0.4 s later. A song the
  lookup does not know gives the 404 of the real service. `/play` needs `song`: the mock has no
  album-only playback.
- `format` is always `lossless`, 44.1 kHz / 16 bit.
- `POST /mock/hang?call=<phase>` makes `/status` report `agentHung: true` with that
  `agentPhase`; `POST /mock/hang` without `call` recovers.
- `MOCK_SPEED=<factor>` makes the clock run faster (for end-of-track tests);
  `MOCK_AUTOPLAY=1` starts an unrelated track when one ends, like the engine's autoplay.
- `/quit` resets the mock but keeps it running, so one mock can serve several tests.
- The mock does not check methods or parameters as strictly as the service: every command
  answers `{"ok":true,"hresult":"00000000"}` (no `quality` field), any method is accepted, and
  error bodies are shorter (`{"ok":false}` for 401 and unknown paths). `/next` and `/previous`
  answer like the other commands but change nothing: the mock has no queue.

## Client examples

Find catalogue IDs with the public iTunes Search API (no account needed): `trackId` is the song
ID, `collectionId` the album ID.

```sh
curl -s 'https://itunes.apple.com/search?term=daft+punk+one+more+time&entity=song&limit=1&country=us' |
    python3 -c 'import json,sys; r=json.load(sys.stdin)["results"][0]; print(r["trackId"], r["collectionId"])'
```

### curl

```sh
TOKEN_FILE="$AM_PREFIX/drive_c/users/$USER/AppData/Local/AppleMusicWrapper/token"
api() { curl -sS -m 70 -X "$1" -H "Authorization: Bearer $(cat "$TOKEN_FILE")" "http://127.0.0.1:${AM_PORT:-7654}$2"; echo; }

api GET  /status
api POST "/play?song=<songId>&album=<albumId>"
api POST /pause
api POST "/volume?v=0.5"
api POST "/quality?q=lossless"
```

(`AM_PREFIX` must be the prefix the service runs in; see [configuration.md](configuration.md)
for its default.)

### JavaScript (Node 18+ or Electron's main process)

```js
import { readFile } from 'node:fs/promises';

const port = process.env.AM_PORT || 7654;
const tokenFile = `${process.env.AM_PREFIX}/drive_c/users/${process.env.USER}/AppData/Local/AppleMusicWrapper/token`;
let token = null;

async function api(method, path) {
  for (let attempt = 0; attempt < 2; attempt++) {
    token ??= (await readFile(tokenFile, 'utf8')).trim();
    const res = await fetch(`http://127.0.0.1:${port}${path}`, {
      method,
      headers: { Authorization: `Bearer ${token}` },
      signal: AbortSignal.timeout(70000),
    });
    if (res.status === 401 && attempt === 0) { token = null; continue; }  // service restarted
    return { status: res.status, body: await res.json() };
  }
}

// play a song in its album, then wait until the engine reports it
async function play(songId, albumId) {
  const r = await api('POST', `/play?song=${songId}&album=${albumId}`);
  if (r.status !== 200) throw new Error(`play failed: ${r.status} ${JSON.stringify(r.body)}`);
  for (let i = 0; i < 40; i++) {
    const { body: st } = await api('GET', '/status');
    if (st.playing && st.nowPlaying?.songId === String(songId)) return st;
    await new Promise((ok) => setTimeout(ok, 250));
  }
  throw new Error('track did not start');
}
```

A connection refused error means the service is not running; start it with `bin/amd` (it
takes several seconds until the API answers).
