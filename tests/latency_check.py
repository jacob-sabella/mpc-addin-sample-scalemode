#!/usr/bin/env python3
"""Latency of the hits `padctl lat scale|page <n> <period ms>` plays, in a recording of the Sample's USB audio.

    latency_check.py rec.wav period_s [max_ms]

The hits sit on an exact grid; every other one has Scale (or the SCALE page) flipped. Prints each hit's delay against the grid
(fitted to the even hits) and the odd-minus-even mean: what the flip costs. Exit 1 when that is more than max_ms (default 10).
A hit is the start of a rise of 0.25 (of the peak) in the 8 ms envelope within 20 ms, at least 0.7 period after the last."""
import sys, wave
import numpy as np

path, per = sys.argv[1], float(sys.argv[2]); lim = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
w = wave.open(path); sr = w.getframerate()
dt = {2: np.int16, 4: np.int32}[w.getsampwidth()]
x = np.abs(np.frombuffer(w.readframes(w.getnframes()), dtype=dt).astype(float).reshape(-1, w.getnchannels()).mean(1))
x /= (x.max() or 1)
ms, win = int(0.001 * sr), int(0.008 * sr)
env = np.array([x[max(0, i - win):i + 1].max() for i in range(0, len(x), ms)])
rise = env[20:] - env[:-20]; on = []; i = 0
while i < len(rise):
    if rise[i] > 0.25:
        j = i + int(np.argmax(rise[i:i + 30])); on.append((j + 10) * 0.001); i = j + int(0.7 * per * 1000)
    else:
        i += 1
on = np.array(on)
if len(on) < 4: sys.exit(f'{len(on)} hits found')
k = np.arange(len(on)); t0 = np.median(on[0::2] - k[0::2] * per); d = (on - t0 - k * per) * 1000
print(f'{len(on)} hits; delay against the grid, ms')
print('  as found:', ' '.join(f'{v:+.0f}' for v in d[0::2]))
print('  flipped: ', ' '.join(f'{v:+.0f}' for v in d[1::2]))
cost = d[1::2].mean() - d[0::2].mean()
print(f'flipped minus as found: {cost:+.1f} ms (sd {d[0::2].std():.1f} / {d[1::2].std():.1f} ms)')
sys.exit(1 if abs(cost) > lim else 0)
