#!/usr/bin/env python3
"""Pitch of each pad hit in a recording of MPC's audio (the Sample's USB audio), relative to the first hit.

    pitch_check.py clicks   rec.wav count [expected]
    pitch_check.py spectrum rec.wav first_s|auto step_s count [expected]

expected: semitones per hit, comma separated; exit 1 when a hit is more than 0.25 semitone off.

Repitching a sample by r plays it r times faster, so:
- clicks: for a sample with two or more transients (drums, clicks). Hits are told apart by silence (0.6 s), and the
  time from a hit's first transient to its last shrinks by r.
- spectrum: for a pitched sample (tests use a saw at C3 with a short fade), hits every step_s from first_s, or with auto
  each at its attack (the level jumps 2.5 times within 30 ms, at least 0.7 step_s after the last). Each hit's log-magnitude spectrum is
  matched against the first one's over stretches 2^(s/12), s from -12 to +36 in 0.05 steps."""
import sys, wave
import numpy as np

mode, path = sys.argv[1], sys.argv[2]
w = wave.open(path); sr = w.getframerate()
dt = {2: np.int16, 4: np.int32}[w.getsampwidth()]
x = np.frombuffer(w.readframes(w.getnframes()), dtype=dt).astype(float).reshape(-1, w.getnchannels()).mean(1)
x /= (np.abs(x).max() or 1)

if mode == 'clicks':
    n = int(sys.argv[3]); exp_arg = sys.argv[4] if len(sys.argv) > 4 else None
    loud = np.where(np.abs(x) > 0.25)[0]; tr = []
    for i in loud:
        if not tr or i - tr[-1] > int(0.03 * sr): tr.append(i)
    t = np.array(tr) / sr
    if not len(t): sys.exit('no transients')
    hits = [[t[0]]]
    for v in t[1:]:
        (hits.append([v]) if v - hits[-1][-1] > 0.6 else hits[-1].append(v))
    if len(hits) < n: sys.exit(f'{len(hits)} hits found, {n} expected')
    hits = hits[-n:]   # a sound still ringing when the recording started comes first
    span = [h[-1] - h[0] for h in hits]
    if span[0] <= 0: sys.exit('the first hit has one transient: use spectrum')
    semis = [12 * np.log2(span[0] / s) if s > 0 else float('nan') for s in span]
elif mode == 'spectrum':
    step, n = float(sys.argv[4]), int(sys.argv[5]); exp_arg = sys.argv[6] if len(sys.argv) > 6 else None
    if sys.argv[3] == 'auto':
        fr = int(0.01 * sr); env = np.array([np.abs(x[i:i + fr]).max() for i in range(0, len(x) - fr, fr)])
        starts = []
        for i in range(3, len(env)):
            if env[i] > 0.2 and env[i] > 2.5 * env[i - 3] and (not starts or i * 0.01 - starts[-1] > 0.7 * step):
                starts.append(i * 0.01)
        if len(starts) < n: sys.exit(f'{len(starts)} hits found, {n} expected')
        starts = [v + 0.02 for v in starts[-n:]]   # past the attack
    else:
        starts = [float(sys.argv[3]) + k * step for k in range(n)]
    N = 1 << 15; freqs = np.fft.rfftfreq(N, 1 / sr); grid = np.geomspace(25, 8000, 2400)

    def spectrum(k):
        a = int(starts[k] * sr); seg = x[a:a + int(step * 0.6 * sr)]
        seg = np.pad(seg, (0, max(0, N - len(seg))))[:N] * np.hanning(N)
        return np.interp(grid, freqs, np.log1p(np.abs(np.fft.rfft(seg))))

    ref = spectrum(0); semis = []
    for k in range(n):
        s = spectrum(k); s = s - s.mean(); best, bs = -1e18, 0.0
        for st in np.arange(-12, 36.001, 0.05):
            tt = np.interp(grid / 2 ** (st / 12), grid, ref, left=0, right=0)
            c = np.dot(tt - tt.mean(), s)
            if c > best: best, bs = c, st
        semis.append(bs)
else:
    sys.exit(__doc__)

exp = [float(v) for v in exp_arg.split(',')] if exp_arg else None
bad = 0
for k, v in enumerate(semis):
    ok = exp is None or abs(v - exp[k]) <= 0.25
    bad += not ok
    print(f'hit {k:2d}: {v:+6.2f} st' + ('' if exp is None else f'  expected {exp[k]:+.0f}  {"ok" if ok else "FAIL"}'))
sys.exit(1 if bad else 0)
