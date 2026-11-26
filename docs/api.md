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
- The request line and headers must fit in 4 KB and arrive within 5 s in all.

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
| `POST /play` | `album=<id>` (required), `song=<id>`, `storefront=<cc>`, `queue=single`, `next=<id>` + `nextAlbum=<id>` | `{"ok":true,"hresult":"00000000","loadMs":812}` |
| `POST /pause` | none | `{"ok":true,"hresult":"00000000"}` |
| `POST /resume` | none | same |
| `POST /stop` | none | same |
| `POST /next` | `next=<id>` + `nextAlbum=<id>`, `storefront=<cc>` (optional) | same; with `next`: as `/play` with `next` |
| `POST /previous` | none | same |
| `POST /seek` | `ms=<position>` | same |
| `POST /volume` | `v=<0..1>` | same |
| `POST /quality` | `q=aac\|lossless\|hires` | `{"ok":true,"hresult":"00000000","quality":"lossless"}` |
| `POST /crossfade` | `on=0\|1` (required), `s=<seconds 1..12>` | `{"ok":true,"hresult":"00000000","crossfade":{"on":true,"seconds":6}}` |
| `POST /enqueue` | `song=<id>`, `album=<id>` (both required), `storefront=<cc>`, `at=next\|end` | `{"ok":true,"hresult":"00000000","queueItemId":"3099","loadMs":1163}` |
| `POST /dequeue` | `id=<queueItemId>` | `{"ok":true,"hresult":"00000000","removed":1}` |
| `POST /autoplay` | `on=0\|1` | `{"ok":true,"hresult":"00000000","autoplay":false}` |
| `POST /quit` | none | `{"ok":true}`, then the service exits |

`/crossfade`, `/enqueue`, `/dequeue`, `/autoplay` and the status fields `crossfade`,
`autoplay`, `upNext` and `nowPlaying.queueItemId` are newer than the rest. An older service
answers these endpoints with `404 {"error":"unknown endpoint"}` and lacks the fields, so a
client should feature-detect them (a 404 with that body, or `upNext` missing from the status).

Details:

- **`/play`**: `album` and `song` are numeric catalogue IDs.
  - `song` + `album`: plays the album starting at the song (the queue continues with the
    album). With `queue=single`: the song alone, then the app's autoplay.
  - `album` alone: the whole album from the first track (`queue` is ignored).
  - `storefront`: the two-letter country code of the catalogue, lowercase (`us`, `gb`, `jp`,
    ...). Default: the service's `AM_STOREFRONT`, else `us`. A value that is not all
    lowercase letters is replaced by `us`.
  - `loadMs`: milliseconds until playback was requested.
  - `next` and `nextAlbum` (both numeric, only with a `song`): a song to queue right behind the
    played one ("Play Next", as `/enqueue`), so that its change into it can be crossfaded: the agent
    decides about a crossfade when it starts a track, and a song queued later only counts after a
    pause and a play. The engine pauses the played song as soon as it is the current item, queues
    the next one, then pauses and plays (see `/next`). The answer comes after that (the enqueue's
    0.7-2.6 s more; the song may play for a moment first) and adds `nextQueueItemId` (as
    `/enqueue`'s `queueItemId`, `""` if it did not show up in time), or `"nextQueued":false` when the
    next song could not be queued (the played one plays anyway). `amd.log` has an `API play next=` line.
  - The response comes when playback has been *requested*, not when sound starts; see
    "Timing".
- **`/pause`, `/resume`, `/stop`, `/next`, `/previous`** call the player directly. `ok` and
  `hresult` say whether the engine accepted the call, not that its effect is complete; read
  `/status` to see the result.
- **`/next`** also waits until the next item plays (3 s at most for the item to change; none
  when nothing is next). Right after a skip the agent can show the new item as playing and stay
  at 0 ms without sound (seen on every skip that came right after a `/pause`); the engine then
  pauses and plays it (once at once when it was paused before, else after 1.5 s at 0 ms; twice at
  most). So `/next` from a paused player plays the next item. It takes about the time of the
  item change, plus 0.2 s for a pause and play; `amd.log` has an `API next:` line for each.
  With `next` and `nextAlbum`: that song is queued behind the new item before it goes on, as with
  `/play` (while the pause holds; the answer has `loadMs`, `nextQueueItemId` or `"nextQueued":false`).
  Without it, a skip into an item with nothing queued behind it cannot crossfade at that item's
  end (measured: switched 0.6 s before the end with B queued right after the skip, 8.4 s with `next=`).
- **`/seek`**: `ms` is the position in milliseconds from the start of the track. A missing
  value seeks to 0.
- `/resume`, `/pause`, `/stop`, `/previous`, `/next`, `/seek` and `/volume` answer once the app has run the
  command, after 0.5 s at the latest (measured 6-113 ms), so commands sent one after the other keep their order.
- **`/volume`**: `v` is clamped to 0..1. A missing value sets 0.
- **`/quality`**: the streaming quality setting. It applies to tracks started afterwards and
  survives restarts (the agent keeps it in its own preferences). `quality` in the response is
  the value read back.
- **`/crossfade`**: the app's crossfade setting (`IPlaybackSettings` CrossfadeSongs and
  CrossfadeValue; the value is in seconds, the app's default is 6). `s` is optional and keeps
  the current length when missing. The response reads the setting back. It survives restarts
  like the quality. Only some transitions are crossfaded: see "Queue and seamless playback".
- **`/enqueue`**: "Play Next" (or, with `at=end`, "Add to Queue") of a catalogue song: by default it is added
  right after the current item (section `next-added`) and what plays is not interrupted. `song`, `album` and `storefront`
  are as for `/play`. `queueItemId` is the new item's ID in the queue (as in `upNext` and
  `nowPlaying.queueItemId`), or `""` if it did not show up in the queue within 5 s. Like the
  app's own Play Next, a later `/enqueue` goes **before** the earlier ones: to queue several
  songs in order, use `at=end`. Needs something playing or paused (409 otherwise).
  A `/play` replaces the queue and drops the enqueued items.
  - `at=end` (default `at=next`) runs the app's "Add to Queue": the song goes
    to the end of `next-added`, behind earlier items (measured after `queue=single`), so a UI can stay
    two items ahead and have AAC items crossfaded (see "Queue and seamless playback"). Its order
    against the rest of a `/play`'ed album is not measured.
  - `at=end` needs the English menu title "Add to Queue", otherwise 500 `80004001`: unlike Play Next
    it has no fallback for other UI languages, as the item in its place would be Create Station.
  - `upNext` shows only 5 items, so follow the item by `queueItemId`. With `""` the song may still
    have been added, so do not resend blindly.
- **`/dequeue`**: with `id`, removes that queue item; without, every item after the current
  one in the `next-added` and `next` sections (not autoplay, not history). `removed` is the
  number of items asked to be removed. It does not touch what plays: the current item is never
  removed, also when its `id` is given (`removed` 0) or when the sections still list it as up
  next just after a track change.
- **`/autoplay`**: the engine's autoplay (similar songs after the end of the queue). Off: the
  `autoplay` section empties and playback stops after the last queued item. Kept across
  restarts. `autoplay` in the response is read back.
- **`/quit`**: answers first, then stops playback (about 1.5 s) and ends the service.
  `bin/amd` then ends the prefix's Wine session, including the agent.

### Errors

| Status | Body | When |
|---|---|---|
| 400 | `{"error":"bad request"}` | malformed request line |
| 400 | `{"ok":false,"error":"need album=<id> (and song=<id> for a single song), numeric"}` | `/play` without a numeric `album`, or a non-numeric `song`, or before the main page has loaded |
| 400 | `{"ok":false,"error":"need q=aac\|lossless\|hires"}` | `/quality` with another value, or before the playback settings exist |
| 400 | `{"ok":false,"error":"need on=0\|1 and optionally s=<seconds 1..12>"}` | `/crossfade` with another value, or before the playback settings exist |
| 400 | `{"ok":false,"error":"need song=<id>&album=<id>, numeric"}` | `/enqueue` without both IDs, or before the main page has loaded |
| 400 | `{"ok":false,"error":"at must be next or end"}` | `/enqueue` with another `at` |
| 400 | `{"ok":false,"error":"id must be a numeric queueItemId"}` | `/dequeue` |
| 400 | `{"ok":false,"error":"need on=0\|1"}` | `/autoplay` |
| 401 | `{"error":"missing or wrong token"}` | see "Authentication" |
| 404 | `{"ok":false,"error":"item not found on the album page","pageError":false,"loadMs":10012}` | `/play`, `/enqueue`: the album page did not contain the song (or the album) within about 10 s; `pageError` is the page's own error flag |
| 404 | `{"error":"unknown endpoint"}` | any other path (a POST, once the player is ready; a GET gets 405, a POST before then 503) |
| 405 | `{"error":"use POST (GET only for /status)"}` | a method other than POST (except `GET /status`) |
| 409 | `{"ok":false,"error":"nothing playing"}` | `/enqueue` with no current item |
| 500 | `{"ok":false,"hresult":"8000ffff"}` (plus the fields of the success response) | the engine returned a failure HRESULT (`/enqueue`: `80004001` when the song's context menu has no Play Next, or no Add to Queue) |
| 503 | `{"error":"player not ready"}` | any POST except `/quit` before the engine has a player |
| 503 | `{"error":"the service is quitting"}` | a POST that was waiting when `/quit` ran |
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
    "durationMs": 320357,
    "queueItemId": "2842"
  },
  "crossfade": { "on": false, "seconds": 6 },
  "autoplay": true,
  "upNext": [
    { "songId": "1065973712", "queueItemId": "3099", "section": "next-added" },
    { "songId": "850583587", "queueItemId": "2871", "section": "autoplay" }
  ],
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
| `crossfade` | object | the crossfade **setting**: `on` (boolean), `seconds` (integer); `{"on":false,"seconds":0}` before the settings exist |
| `autoplay` | boolean | the engine's autoplay is on (`PlayQueue.AutoPlayActive`) |
| `upNext` | array | the next items after the current one in play order, at most 5: `songId` (catalogue ID, `""` for an item the engine is still resolving), `queueItemId`, `section` (`next-added`: Play Next and Add to Queue items, `next`: the rest of the album, `autoplay`). `[]` when nothing is playing or nothing follows |
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
| `positionMs`, `durationMs` | integer | position and duration in milliseconds. The engine updates the position about once a second |
| `queueItemId` | string | the item's ID in the engine's queue (`PlayQueueItemID`, decimal), `""` if it has none. It changes when the same song is queued again |

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
- Polling `/status` every 250-500 ms is fine; the status path does not leak (3000 requests in
  a row, the queue included: no growth of the service's memory) and answers in a few
  milliseconds.
- **`/enqueue` takes about 0.7-1.4 s** (the album page loads, then the queue shows the item
  about 0.5 s after Play Next). The worst case is about 20 s (up to 10 s for the page, up to
  5 s for the context menu, up to 5 s for the item to reach the queue); a song that is not on
  the page answers 404 after about 11-13 s. Like every request it holds up the others, except `/status`,
  which is answered meanwhile; allow more than 20 s for the `/enqueue` itself. Send it well before the current track ends
  (20-30 s ahead). `at=end` takes as long (0.9-1.6 s, eight calls).
- A UI that advances its own queue with one `/play` per item has a gap of the deep-link load
  time between tracks. With `/enqueue` the engine continues by itself (next section).

## Queue and seamless playback

The engine plays its own queue: history, the current item, `next-added` (Play Next, Add to Queue), `next`
(the rest of the album) and `autoplay`. When an item ends, the engine starts the next one by
itself; a UI that wants seamless transitions enqueues its next track with `/enqueue` while the
current one plays and then follows `nowPlaying` instead of sending `/play`.

- `nowPlaying.songId` and `queueItemId` switch to the next item when its audio starts, i.e.
  when the previous item has ended (measured: within about 0.1 s of the first sound of the new
  item, polling every 100 ms). `upNext` follows a moment later; the current item is not listed
  in it meanwhile.
- **Gapless**: tracks of the same audio format play from the same audio stream without a break, in
  an album (Pink Floyd, "Brain Damage" into "Eclipse": no digital silence of 5 ms or more) and
  across albums (Nirvana, "Territorial Pissings", into Daft Punk, "Get Lucky", both 24-bit ALAC at
  44.1 kHz: 13 ms). With the `aac` quality the four pairs of different albums tried all shared one stream.
- Between lossless tracks of different formats (seen: 44.1 and 48 kHz, and 16 and 24 bit) the engine
  closes the track's audio stream and opens a new one: measured 0.16-0.9 s of digital silence, what
  the songs themselves have included.
- **Crossfade** happens only with the `aac` quality, when the next item is a catalogue item (not
  autoplay; by the agent's code also not the next track of the same album) that was queued **before
  the current track started**: the agent decides then, and an `/enqueue` during the track comes too late
  for that track's end. Measured, one transition each: the next track started (`songId` switched) 6 s
  before the other's end with 8 s set and 8 s before with 12 s, in one stream and without a
  silence. To have the item after next queued in time, stay two items ahead with `at=end`:
  items added so before the current one started switched about 6.9 s and 5.6 s early (8 s set, one
  transition each, by the last reported position); one added while the current track played switched
  about 1 s before the end. With `lossless`
  (measured) and `hires` (same player) the engine does not crossfade
  (its CoreMedia player never reads the setting).

## Concurrency

The engine may only be called from its UI thread. Connections are accepted concurrently (one
short-lived thread each, at most 16 at once, more wait in the listen backlog; a request head that does
not arrive within 5 s is dropped), but the requests are handed to the main thread, which runs them
**one at a time, in arrival order**. While a `/play` or any other request runs, a `/status` from
another client waits until it is done. The exception is **`/enqueue`**: while one runs, queued
`GET /status` requests are answered (one at a time, from the enqueue's own message pump) and
everything else keeps its place until the `/enqueue` ends. `/status` is deliberately not answered
during `/play`: it must not show the previous track while a play changes it. A request that waits
more than 60 s gets a 503 (it no longer runs if it had not started); one whose client has gone away still runs. Once a
`/quit` has been handled, other POSTs get a 503. Keep requests short and
do not poll from several timers in one client.

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
- Like the service it handles **one request at a time**: a `/status` sent while a `/play` runs
  waits for it, but one sent while an `/enqueue` runs is answered at once.
- `/play` waits 0.9 s, looks the song up in the public iTunes Search API (network access
  needed; `storefront` default `AM_STOREFRONT`, else `us`) and starts it 0.4 s later. A song the
  lookup does not know gives the 404 of the real service (at once, not after about 10 s).
  `/play` needs `song`: the mock has no album-only playback. It replaces the queue, as with
  `queue=single`, and drops enqueued items.
- `/enqueue` checks its parameters like the service (400, 409 `nothing playing`), then takes
  about 0.75 s plus the lookup (0.4 s for the page, 0.35 s until the item is in the queue) and
  answers with a numeric `queueItemId`. The new item goes first in `upNext` (`next-added`),
  before earlier ones. With `at=end` it goes last.
- `/dequeue`, `/crossfade` and `/autoplay` take and check the parameters of the service and
  answer with the same fields and errors (`removed` counts the items asked for). The crossfade
  setting is kept and reported but, as with lossless on the engine, has **no effect**: the next
  item starts at the end of the current one, and `nowPlaying.songId` switches then.
- `nowPlaying.positionMs` advances in steps of a second after a play, seek or resume, as the
  engine's does, and stops at `durationMs`. At the end the next queued item starts at once
  (playing throughout, as the engine's status shows a gapless transition), else the autoplay
  item, else playback stops (`nowPlaying` null).
- `format` is always `lossless`, 44.1 kHz / 16 bit.
- `POST /mock/hang?call=<phase>` makes `/status` report `agentHung: true` with that
  `agentPhase`; `POST /mock/hang` without `call` recovers.
- `MOCK_SPEED=<factor>` makes the clock run faster (for end-of-track tests).
- `MOCK_AUTOPLAY=1` starts with the engine's autoplay on (default off; `/autoplay` changes it):
  `upNext` then ends with one `autoplay` item (song ID `1`), which plays after the last queued
  item.
- `MOCK_NO_SEAMLESS=1`, or `POST /mock/seamless?on=0` at run time (`on=1` back), makes it an
  older service: `/crossfade`, `/enqueue`, `/dequeue` and `/autoplay` answer
  `404 {"error":"unknown endpoint"}`, and the status has no `crossfade`, `autoplay`, `upNext`
  or `nowPlaying.queueItemId`.
- `/next` goes on to the next queued item (or the autoplay item, or stops); `/previous` is
  accepted but changes nothing (no history).
- `/quit` resets the mock but keeps it running, so one mock can serve several tests.
- The mock does not check methods, or the parameters of the older commands, as strictly as the
  service: those answer `{"ok":true,"hresult":"00000000"}` (`/quality` without its `quality`
  field), any method is accepted, and unknown paths answer `404 {"error":"unknown endpoint"}`
  whatever the method.

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
