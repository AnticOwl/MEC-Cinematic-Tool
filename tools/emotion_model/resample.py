# Reference resampler to 16 kHz: band-limited (windowed sinc, Kaiser beta 8.6), cutoff 0.475 x the lower rate,
# 32 zero crossings each side, every tap computed exactly (no table). The helper's C++ resampler must agree with it.
import numpy as np

ZEROS = 32
BETA = 8.6
ROLLOFF = 0.95


def to16k(x, fin, fout=16000):
    x = np.asarray(x, np.float64)
    if fin == fout:
        return x.astype(np.float32)
    cut = 0.5 * min(fin, fout) * ROLLOFF          # Hz
    step = fin / (2.0 * cut)                       # input samples between zero crossings
    half = ZEROS * step                            # kernel half-width in input samples
    n_out = int(np.floor(len(x) * fout / fin))
    out = np.zeros(n_out)
    gain = 2.0 * cut / fin
    for a in range(0, n_out, 4096):
        n = np.arange(a, min(a + 4096, n_out))
        pos = n * (fin / fout)
        lo = np.ceil(pos - half).astype(np.int64)
        taps = int(np.ceil(2 * half)) + 1
        k = lo[:, None] + np.arange(taps)[None, :]
        d = pos[:, None] - k
        inside = (np.abs(d) < half) & (k >= 0) & (k < len(x))
        u = d / half
        w = np.where(np.abs(u) < 1, np.i0(BETA * np.sqrt(np.clip(1 - u * u, 0, 1))) / np.i0(BETA), 0)
        h = gain * np.sinc(d / step) * w
        xs = x[np.clip(k, 0, len(x) - 1)]
        out[n - a + a] = (np.where(inside, h * xs, 0)).sum(1)
    return out.astype(np.float32)
