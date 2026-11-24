// SPDX-License-Identifier: LGPL-2.1-or-later
// Silent stand-in for the wrapper service: same API, a clock instead of audio.
// usage: node test/mock-daemon.mjs [port=7699]   token: "mocktoken" (Authorization: Bearer mocktoken)
// Environment: AM_STOREFRONT (default us: catalogue lookups for /play without &storefront=),
//              MOCK_SPEED (clock multiplier), MOCK_AUTOPLAY=1 (the engine's autoplay on at start:
//              an unrelated track follows the end of the queue), MOCK_NO_SEAMLESS=1 (an older
//              service: no /crossfade, /enqueue, /dequeue, /autoplay and none of their status fields;
//              POST /mock/seamless?on=0|1 switches at run time).
// Like the service it handles one request at a time (a /play holds up /status), except that a
// /status is answered while an /enqueue runs; it reports
// positionMs in steps of about a second, and ignores crossfade: the setting is kept, but the next
// item starts at the end of the current one, as the engine does under Wine (docs/api.md).
import http from 'http';
const PORT = +process.argv[2] || 7699;
const SPEED = +process.env.MOCK_SPEED || 1;
const STOREFRONT = process.env.AM_STOREFRONT || 'us';
let hung = null;   // POST /mock/hang?call=<phase> (empty: recovered): the agent watchdog's state
let quality = 'lossless', vol = 0.05, cur = null, playing = false, paused = false, pos = 0, at = 0;
let seamless = !process.env.MOCK_NO_SEAMLESS;
let autoplay = !!process.env.MOCK_AUTOPLAY, crossfade = { on: false, seconds: 6 };
let upNext = [], auto = null, qid = 1000;   // next-added items, the autoplay item; qid: the last queue item ID
const now = () => Date.now();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const elapsed = () => (now() - at) * SPEED;
const position = () => (cur && playing ? Math.min(cur.dur, pos + elapsed()) : pos);
// what /status shows: the engine updates the position about once a second after a play, seek or resume
const shown = () => (cur && playing ? Math.min(cur.dur, pos + Math.floor(elapsed() / 1000) * 1000) : pos);
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const start = (t) => { cur = t; pos = 0; at = now(); playing = true; paused = false; auto = null; };
const autoItem = () => (auto ??= { title: 'Unrelated Autoplay Track', dur: 180000, id: '1', artist: 'Someone', album: 'Something', qid: String(++qid) });
function advance(why) {
  if (seamless && upNext.length) { const n = upNext.shift(); log(why, 'into', n.title); start(n); }
  else if (autoplay) { log(why, 'AUTOPLAY kicks in'); start(autoItem()); }
  else { log(why, '-> stopped'); playing = paused = false; pos = 0; cur = null; }
}
function tick() {
  if (cur && playing && pos + elapsed() >= cur.dur) advance(crossfade.on ? 'end (crossfade on, no effect)' : 'end, gapless');
}
setInterval(tick, 50);
// the public iTunes lookup stands in for the album page; null when the song is not found
async function lookup(q) {
  const j = await (await fetch(`https://itunes.apple.com/lookup?id=${q.song}&country=${q.storefront || STOREFRONT}`)).json();
  const t = j.results[0];
  if (!t) return null;
  if (String(t.collectionId) !== q.album) log('!! album mismatch', q.album, t.collectionId);
  return { title: t.trackName, dur: t.trackTimeMillis, id: String(t.trackId), artist: t.artistName, album: t.collectionName, genre: t.primaryGenreName, date: t.releaseDate, qid: String(++qid) };
}
const digits = (v) => /^\d+$/.test(v ?? '');
const NOT_FOUND = (ms) => ({ ok: false, error: 'item not found on the album page', pageError: false, loadMs: ms });
async function handle(req, res) {
  const u = new URL(req.url, 'http://x');
  const send = (code, o) => { res.writeHead(code, { 'content-type': 'application/json' }); res.end(JSON.stringify(o)); };
  if (req.headers.authorization !== 'Bearer mocktoken') return send(401, { error: 'missing or wrong token' });
  if (u.pathname === '/status') {
    tick();
    const st = { ready: true, signedIn: true, playing, paused, volume: vol, quality,
      nowPlaying: cur ? { title: cur.title, songId: cur.id, artist: cur.artist, album: cur.album, genre: cur.genre, releaseDate: cur.date, format: 'lossless', sampleRate: '44.1', bitDepth: '16', positionMs: Math.round(shown()), durationMs: cur.dur } : null, agentHung: !!hung, agentPhase: hung };
    if (seamless) {
      if (st.nowPlaying) st.nowPlaying.queueItemId = cur.qid || '';
      const up = cur ? [...upNext.map((t) => ({ t, section: 'next-added' })), ...(autoplay ? [{ t: autoItem(), section: 'autoplay' }] : [])] : [];
      Object.assign(st, { crossfade, autoplay, upNext: up.slice(0, 5).map(({ t, section }) => ({ songId: t.id, queueItemId: t.qid, section })) });
    }
    return send(200, st);
  }
  log(req.method, req.url);
  const q = Object.fromEntries(u.searchParams);
  if (!seamless && ['/crossfade', '/enqueue', '/dequeue', '/autoplay'].includes(u.pathname)) return send(404, { error: 'unknown endpoint' });
  switch (u.pathname) {
    case '/play': {
      await sleep(900);
      try {
        const t = await lookup(q);
        if (!t) return send(404, NOT_FOUND(900));
        // queue=single: the song alone replaces the queue, enqueued items included
        setTimeout(() => { upNext = []; start(t); log('now playing', cur.title, cur.dur); }, 400);
        return send(200, { ok: true, hresult: '00000000', loadMs: 900 });
      } catch (e) { return send(500, { ok: false, error: String(e) }); }
    }
    case '/enqueue': {   // Play Next: right after the current item, before earlier ones; what plays goes on
                         // (at=end: Add to Queue, behind the earlier ones)
      if (!digits(q.song) || !digits(q.album)) return send(400, { ok: false, error: 'need song=<id>&album=<id>, numeric' });
      if (q.at && !['next', 'end'].includes(q.at)) return send(400, { ok: false, error: 'at must be next or end' });
      if (!cur) return send(409, { ok: false, error: 'nothing playing' });
      await sleep(400);   // the album page
      try {
        const t = await lookup(q);
        if (!t) return send(404, NOT_FOUND(400));
        await sleep(350); // Play Next until the item shows in the queue
        if (q.at === 'end') upNext.push(t); else upNext.unshift(t);
        log('enqueued', t.title, t.qid);
        return send(200, { ok: true, hresult: '00000000', queueItemId: t.qid, loadMs: 750 });
      } catch (e) { return send(500, { ok: false, error: String(e) }); }
    }
    case '/dequeue': {   // that item, or everything after the current one but autoplay; removed: how many were asked for
      if (q.id && !digits(q.id)) return send(400, { ok: false, error: 'id must be a numeric queueItemId' });
      const removed = q.id ? +(q.id !== cur?.qid) : upNext.length;   // never the current item
      upNext = q.id ? upNext.filter((t) => t.qid !== q.id) : [];
      return send(200, { ok: true, hresult: '00000000', removed });
    }
    case '/crossfade': {
      if ((q.on !== '0' && q.on !== '1') || (q.s && !(digits(q.s) && +q.s >= 1 && +q.s <= 12)))   // s= empty: kept, as the service does
        return send(400, { ok: false, error: 'need on=0|1 and optionally s=<seconds 1..12>' });
      crossfade = { on: q.on === '1', seconds: q.s ? +q.s : crossfade.seconds };
      return send(200, { ok: true, hresult: '00000000', crossfade });
    }
    case '/autoplay':
      if (q.on !== '0' && q.on !== '1') return send(400, { ok: false, error: 'need on=0|1' });
      autoplay = q.on === '1';
      return send(200, { ok: true, hresult: '00000000', autoplay });
    case '/pause': if (playing) { pos = position(); playing = false; paused = true; } break;
    case '/resume': if (cur && !playing) { at = now(); playing = true; paused = false; } break;
    case '/stop': cur = null; playing = paused = false; pos = 0; break;
    case '/next': if (cur) advance('/next'); break;
    case '/previous': break;   // no history in the mock: accepted, nothing changes
    case '/quit': cur = null; playing = paused = false; pos = 0; upNext = []; break;   // the mock keeps running for the next test
    case '/seek': pos = +q.ms || 0; at = now(); break;
    case '/volume': vol = +q.v; break;
    case "/quality": quality = q.q; break;
    case '/mock/hang': hung = q.call || null; break;
    case '/mock/seamless': seamless = q.on !== '0'; if (!seamless) upNext = []; break;
    default: return send(404, { error: 'unknown endpoint' });
  }
  send(200, { ok: true, hresult: '00000000' });
}
// one request at a time, like the service's single dispatch thread; but a GET /status that arrives
// while an /enqueue runs is answered at once (the service does that from the enqueue's message pump).
// During /play and every other request it waits its turn.
let line = Promise.resolve(), running = null;
http.createServer((req, res) => {
  const path = req.url.split('?')[0];
  if (path === '/status' && req.method === 'GET' && running === '/enqueue') {
    handle(req, res).catch((e) => log('!!', e));
    return;
  }
  line = line.then(async () => { running = path; try { await handle(req, res); } finally { running = null; } }).catch((e) => log('!!', e));
}).listen(PORT, '127.0.0.1', () => log('mock on', PORT));
