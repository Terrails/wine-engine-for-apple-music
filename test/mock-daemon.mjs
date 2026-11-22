// SPDX-License-Identifier: LGPL-2.1-or-later
// Silent stand-in for the wrapper service: same API, a clock instead of audio.
// usage: node test/mock-daemon.mjs [port=7699]   token: "mocktoken" (Authorization: Bearer mocktoken)
// Environment: AM_STOREFRONT (default us: catalogue lookups for /play without &storefront=),
//              MOCK_SPEED (clock multiplier), MOCK_AUTOPLAY=1 (a track follows the end of the last).
import http from 'http';
const PORT = +process.argv[2] || 7699;
const SPEED = +process.env.MOCK_SPEED || 1;
const STOREFRONT = process.env.AM_STOREFRONT || 'us';
let hung = null;   // POST /mock/hang?call=<phase> (empty: recovered): the agent watchdog's state
let quality = 'lossless', vol = 0.05, cur = null, playing = false, paused = false, pos = 0, at = 0;
const now = () => Date.now();
const position = () => (cur && playing ? Math.min(cur.dur + 99999, pos + (now() - at) * SPEED) : pos);
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
function tick() {
  if (cur && playing && position() >= cur.dur) {
    if (process.env.MOCK_AUTOPLAY) { log('AUTOPLAY kicks in'); cur = { title: 'Unrelated Autoplay Track', dur: 180000, id: '1', artist: 'Someone', album: 'Something' }; pos = 0; at = now(); }
    else { log('natural end -> stopped'); playing = false; pos = 0; cur = null; }
  }
}
setInterval(tick, 50);
http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  const send = (code, o) => { res.writeHead(code, { 'content-type': 'application/json' }); res.end(JSON.stringify(o)); };
  if (req.headers.authorization !== 'Bearer mocktoken') return send(401, { ok: false });
  if (u.pathname === '/status') {
    tick();
    return send(200, { ready: true, signedIn: true, playing, paused, volume: vol, quality,
      nowPlaying: cur ? { title: cur.title, songId: cur.id, artist: cur.artist, album: cur.album, genre: cur.genre, releaseDate: cur.date, format: 'lossless', sampleRate: '44.1', bitDepth: '16', positionMs: Math.round(position()), durationMs: cur.dur } : null, agentHung: !!hung, agentPhase: hung });
  }
  log(req.method, req.url);
  const q = Object.fromEntries(u.searchParams);
  switch (u.pathname) {
    case '/play': {
      await new Promise((r) => setTimeout(r, 900));
      try {
        const j = await (await fetch(`https://itunes.apple.com/lookup?id=${q.song}&country=${q.storefront || STOREFRONT}`)).json();
        const t = j.results[0];
        if (!t) return send(404, { ok: false, error: 'item not found on the album page' });
        if (String(t.collectionId) !== q.album) log('!! album mismatch', q.album, t.collectionId);
        setTimeout(() => { cur = { title: t.trackName, dur: t.trackTimeMillis, id: String(t.trackId), artist: t.artistName, album: t.collectionName, genre: t.primaryGenreName, date: t.releaseDate }; pos = 0; at = now(); playing = true; paused = false; log('now playing', cur.title, cur.dur); }, 400);
        return send(200, { ok: true, hresult: '00000000', loadMs: 900 });
      } catch (e) { return send(500, { ok: false, error: String(e) }); }
    }
    case '/pause': if (playing) { pos = position(); playing = false; paused = true; } break;
    case '/resume': if (cur && !playing) { at = now(); playing = true; paused = false; } break;
    case '/stop': cur = null; playing = paused = false; pos = 0; break;
    case '/next': case '/previous': break;   // no queue in the mock: accepted, nothing changes
    case '/quit': cur = null; playing = paused = false; pos = 0; break;   // the mock keeps running for the next test
    case '/seek': pos = +q.ms; at = now(); break;
    case '/volume': vol = +q.v; break;
    case "/quality": quality = q.q; break;
    case '/mock/hang': hung = q.call || null; break;
    default: return send(404, { ok: false });
  }
  send(200, { ok: true, hresult: '00000000' });
}).listen(PORT, '127.0.0.1', () => log('mock on', PORT));
