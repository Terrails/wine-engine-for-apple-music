# SPDX-License-Identifier: LGPL-2.1-or-later
# usage: python3 -I dev/gapmon.py <seconds>  (needs numpy, parec; read-only, does not touch playback)
# Reads the service's token from AM_PREFIX (default as in bin/amd-env.sh) and its port from AM_PORT.
# Logs, with timestamps:
#   SILENCE  digital silence >= 5 ms inside music in the default sink's output (its monitor, after
#            mixing), with the track and position from the service. This is what catches dropouts
#            of a second or so: a playback stream that runs dry for longer than a cycle or two does
#            not show it on its own monitor (it shows ~10 ms of zeros and then keeps delivering;
#            a test stream whose writer was stopped for 1 s showed a 10.7 ms GAP on the stream
#            and 864 ms SILENCE on the sink). Silence the song itself has (fades, breaks)
#            is reported too: the position tells them apart.
#   GAP      silence on the newest "Apple Music" (AM_AUDIO_NAME) sink input itself: short underruns (a cycle or two),
#            or silence that is in the song. A SILENCE without a GAP at the same moment is a
#            dropout: the stream ran dry (or something after it did).
#   stream   Apple Music sink inputs appearing, disappearing, corking and uncorking.
import json, os, subprocess, sys, time, datetime, threading, urllib.request, numpy as np
SR, CH = 48000, 2
# the prefix: AM_PREFIX, default as in bin/amd-env.sh (WINEPREFIX is ignored on purpose)
PREFIX = os.environ.get("AM_PREFIX") or os.path.join(
    os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "wine-engine-for-apple-music", "prefix")
AUDIO_NAME = os.environ.get("AM_AUDIO_NAME", "Apple Music")   # the client name bin/amd gives the stream
def ts(): return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
def log(*a): print(ts(), *a, flush=True)

def apple_inputs():
    out = subprocess.run(["pactl", "-f", "json", "list", "sink-inputs"], capture_output=True, text=True).stdout
    try: return [s for s in json.loads(out) if s["properties"].get("application.name") == AUDIO_NAME]
    except Exception: return []

def now_playing():
    """'artist - title @ position/duration' from the service (bin/amctl status), or ''"""
    try:
        tok = open(os.path.join(PREFIX, "drive_c/users", os.environ.get("USER", ""), "AppData/Local/AppleMusicWrapper/token")).read().strip()
        req = urllib.request.Request(f"http://127.0.0.1:{os.environ.get('AM_PORT', '7654')}/status", headers={"Authorization": f"Bearer {tok}"})
        n = json.load(urllib.request.urlopen(req, timeout=1)).get("nowPlaying")
        return f"{n['artist']} - {n['title']} @ {n['positionMs']/1000:.1f}/{n['durationMs']/1000:.0f} s" if n else "nothing playing"
    except Exception: return ""

def watch_streams():
    """Apple Music sink inputs as they appear, go, cork and uncork (pactl subscribe)"""
    seen = {}
    ev = subprocess.Popen(["pactl", "subscribe"], stdout=subprocess.PIPE, text=True)
    for line in ev.stdout:
        if "sink-input" not in line: continue
        now = {s["index"]: s["corked"] for s in apple_inputs()}
        for i in now.keys() - seen.keys(): log(f"stream {i} new{' (corked)' if now[i] else ''}")
        for i in seen.keys() - now.keys(): log(f"stream {i} gone")
        for i in now.keys() & seen.keys():
            if now[i] != seen[i]: log(f"stream {i} {'corked' if now[i] else 'uncorked'}")
        seen = now

def watch_sink():
    """digital silence inside music on the default sink's monitor"""
    sink = subprocess.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()
    log(f"watching {sink}.monitor")
    p = subprocess.Popen(["parec", "-d", sink + ".monitor", "--format=float32le", f"--rate={SR}", f"--channels={CH}", "--latency-msec=50"], stdout=subprocess.PIPE)
    zrun = 0; lvl = 0.0
    while True:
        b = p.stdout.read(SR // 10 * CH * 4)
        if not b: log("sink monitor ended"); return
        m = np.abs(np.frombuffer(b, '<f4')[: len(b) // 8 * 2].reshape(-1, 2)).max(axis=1)
        for i, q in enumerate(m < 1e-6):
            if q: zrun += 1; continue
            if zrun >= SR // 200 and lvl > 0.005:
                log(f"SILENCE {zrun/SR*1000:.0f} ms on the sink ({now_playing()})")
            zrun = 0; lvl = 0.999 * lvl + 0.001 * float(m[i])

threading.Thread(target=watch_streams, daemon=True).start()
threading.Thread(target=watch_sink, daemon=True).start()
end = time.time() + float(sys.argv[1])
while time.time() < end:
    l = sorted(apple_inputs(), key=lambda s: s["index"])   # newest last: the track's client
    if not l: time.sleep(1); continue
    sid, spec = l[-1]["index"], l[-1]["sample_specification"]
    log(f"attach stream {sid} ({spec})")
    p = subprocess.Popen(["parec", f"--monitor-stream={sid}", "--format=float32le", f"--rate={SR}", f"--channels={CH}", "--latency-msec=50"], stdout=subprocess.PIPE)
    hist = np.zeros(0, np.float32)   # last 50 ms of mono abs for level context
    zrun, total, gaps = 0, 0, 0
    while time.time() < end:
        buf = p.stdout.read(SR // 10 * CH * 4)
        if not buf: break
        a = np.frombuffer(buf, '<f4')[: len(buf) // 8 * 2].reshape(-1, 2)
        m = np.abs(a).max(axis=1)
        for i, z in enumerate(m < 1e-6):        # the sink's threshold: silence in the song shows as both GAP and SILENCE
            if z: zrun += 1; continue
            if zrun >= 16:
                ctx = np.concatenate((hist, m[:max(i - zrun, 0)]))[-2400:]
                before = float(np.sqrt(np.mean(ctx ** 2))) if len(ctx) else 0
                after = float(np.sqrt(np.mean(a[i:i+2400] ** 2)))
                if before > 0.005 and after > 0.005:
                    gaps += 1
                    log(f"GAP {zrun} frames ({zrun/SR*1000:.1f} ms) at +{(total+i-zrun)/SR:.2f}s of stream, level {before:.3f}/{after:.3f}")
            zrun = 0
        hist = np.concatenate((hist, m))[-2400:]
        total += len(m)
    p.kill()
    log(f"detach stream {sid}: {total/SR:.0f} s analysed, {gaps} gaps")
