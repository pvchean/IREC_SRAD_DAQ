#!/usr/bin/env python3
"""Python copy of resampleGaps (src/DAQ_Calibration_App.m, combined 1 kHz CSV) and a comparison of the interpolation methods.
No MATLAB and no scipy here: PCHIP (the rule of MATLAB's pchip), makima and a natural cubic spline are written out.

  resample_mirror.py methods          linear / pchip / makima / spline: tones kept, overshoot at an ignition edge, error on a realistic motor
  resample_mirror.py file FILE.BIN    1 kHz grid of a real file (PT 883 SPS, load cell 321 SPS): spans, NaN count, a gap test
"""
import sys
import numpy as np

def pchip(x, y, xq):
    h = np.diff(x); dl = np.diff(y) / h; n = x.size
    d = np.zeros(n)
    if n == 2:
        d[:] = dl[0]
    else:
        for k in range(1, n - 1):
            if dl[k - 1] * dl[k] > 0:
                w1 = 2 * h[k] + h[k - 1]; w2 = h[k] + 2 * h[k - 1]
                d[k] = (w1 + w2) / (w1 / dl[k - 1] + w2 / dl[k])
        def end(h0, h1, d0, d1):
            e = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1)
            if np.sign(e) != np.sign(d0): e = 0.0
            elif np.sign(d0) != np.sign(d1) and abs(e) > 3 * abs(d0): e = 3 * d0
            return e
        d[0] = end(h[0], h[1], dl[0], dl[1]); d[-1] = end(h[-1], h[-2], dl[-1], dl[-2])
    i = np.clip(np.searchsorted(x, xq, side="right") - 1, 0, n - 2)
    s = (xq - x[i]) / h[i]
    h00 = 2 * s ** 3 - 3 * s ** 2 + 1; h10 = s ** 3 - 2 * s ** 2 + s; h01 = -2 * s ** 3 + 3 * s ** 2; h11 = s ** 3 - s ** 2
    return h00 * y[i] + h10 * h[i] * d[i] + h01 * y[i + 1] + h11 * h[i] * d[i + 1]

def natural_spline(x, y, xq):
    n = x.size; h = np.diff(x); A = np.zeros((n, n)); r = np.zeros(n)
    A[0, 0] = A[-1, -1] = 1
    for k in range(1, n - 1):
        A[k, k - 1] = h[k - 1]; A[k, k] = 2 * (h[k - 1] + h[k]); A[k, k + 1] = h[k]
        r[k] = 6 * ((y[k + 1] - y[k]) / h[k] - (y[k] - y[k - 1]) / h[k - 1])
    m = np.linalg.solve(A, r)
    i = np.clip(np.searchsorted(x, xq, side="right") - 1, 0, n - 2)
    a = (x[i + 1] - xq) / h[i]; b = (xq - x[i]) / h[i]
    return a * y[i] + b * y[i + 1] + ((a ** 3 - a) * m[i] + (b ** 3 - b) * m[i + 1]) * h[i] ** 2 / 6

def makima(x, y, xq):
    """Modified Akima cubic (MATLAB 'makima'): the slope at a point is a weighted mean of the two neighbour slopes, the weights come from
    how much the slopes change on each side. No overshoot on a step, and it keeps tones better than pchip or linear."""
    n = x.size; h = np.diff(x)
    mm = np.empty(n + 3); mm[2:-2] = np.diff(y) / h                               # mm[2 + j]: slope of interval j
    mm[1] = 2 * mm[2] - mm[3]; mm[0] = 2 * mm[1] - mm[2]; mm[-2] = 2 * mm[-3] - mm[-4]; mm[-1] = 2 * mm[-2] - mm[-3]
    d = np.zeros(n)
    for i in range(n):
        a1 = abs(mm[i + 3] - mm[i + 2]) + abs(mm[i + 3] + mm[i + 2]) / 2
        a2 = abs(mm[i + 1] - mm[i]) + abs(mm[i + 1] + mm[i]) / 2
        d[i] = (a1 * mm[i + 1] + a2 * mm[i + 2]) / (a1 + a2) if (a1 + a2) > 0 else 0.5 * (mm[i + 1] + mm[i + 2])
    i = np.clip(np.searchsorted(x, xq, side="right") - 1, 0, n - 2); s = (xq - x[i]) / h[i]
    return (2*s**3-3*s**2+1)*y[i] + (s**3-2*s**2+s)*h[i]*d[i] + (-2*s**3+3*s**2)*y[i+1] + (s**3-s**2)*h[i]*d[i+1]

def resample_gaps(t, y, tg):
    """1:1 with resampleGaps: makima, NaN outside the data and inside a gap (more than 10 sample intervals, 50 ms at least)."""
    yi = np.full(tg.shape, np.nan)
    ok = np.isfinite(t) & np.isfinite(y)
    t, y = t[ok], y[ok]
    t, iu = np.unique(t, return_index=True); y = y[iu]
    if t.size < 4: return yi
    inside = (tg >= t[0]) & (tg <= t[-1])
    yi[inside] = makima(t, y, tg[inside])
    dt = np.diff(t); gl = max(0.05, 10 * np.median(dt))
    ip = np.searchsorted(t, tg, side="right") - 1            # the previous sample of each grid time
    big = np.append(dt > gl, False)
    bad = ~inside
    bad[inside] = big[np.clip(ip[inside], 0, big.size - 1)]
    yi[bad] = np.nan
    return yi

def methods():
    rng = np.random.default_rng(7); fs = 321.0
    meths = {"linear": lambda x, y, q: np.interp(q, x, y), "pchip": pchip, "makima": makima, "spline": natural_spline}
    print("1) amplitude kept of a pure tone: load cell fs %.0f Hz, 0.3 ms timing jitter, resampled to 1 kHz" % fs)
    print("   %-6s" % "Hz" + "".join("%9s" % k for k in meths))
    tg = np.arange(2.0, 6.0, 0.001); kept = {}
    for fr in (10, 20, 40, 60, 87, 130):
        t = np.sort(np.arange(0, 8, 1 / fs) + rng.normal(0, 3e-4, int(8 * fs))); y = np.sin(2 * np.pi * fr * t); row = "   %-6d" % fr
        for k, fn in meths.items():
            v = fn(t, y, tg); A = 2 * abs(np.sum(v * np.exp(-2j * np.pi * fr * tg))) / tg.size; kept[(k, fr)] = A; row += "%8.0f%%" % (100 * A)
        print(row)
    truth = lambda x: np.interp(x, [0, 1.0, 1.04, 2.5, 2.54, 4], [0, 0, 2000, 2000, 0, 0])      # ignition and burnout in 40 ms
    t = np.sort(np.arange(0, 4, 1 / fs) + rng.normal(0, 3e-4, int(4 * fs))); tg2 = np.arange(0.5, 3.5, 0.001)
    print("2) ignition 0 -> 2000 N in 40 ms (no tones): highest and lowest value (a method that overshoots invents a peak or a negative thrust)")
    edge = {}
    for k, fn in meths.items():
        v = fn(t, truth(t), tg2); edge[k] = (v.max(), v.min()); print("   %-8s max %7.1f N  min %7.1f N" % (k, v.max(), v.min()))
    real = lambda x: truth(x) + 15 * np.sin(2 * np.pi * 40 * x) + 20 * np.sin(2 * np.pi * 87 * x) + 8 * np.sin(2 * np.pi * 130 * x)
    y = real(t) + rng.normal(0, 3, t.size); pl = (tg2 > 1.3) & (tg2 < 2.3)
    print("3) edge + tones (40 Hz 15 N, 87 Hz 20 N, 130 Hz 8 N) + 3 N noise: rms error against the truth on the plateau")
    err = {}
    for k, fn in meths.items():
        v = fn(t, y, tg2); err[k] = np.sqrt(np.mean((v[pl] - real(tg2)[pl]) ** 2)); print("   %-8s %5.2f N" % (k, err[k]))
    assert edge["makima"][0] <= 2000 + 1e-6 and edge["makima"][1] >= -1e-6 and edge["pchip"][0] <= 2000 + 1e-6, "makima and pchip must not overshoot here"
    assert edge["spline"][0] > 2000 + 5, "the spline is expected to overshoot"
    assert kept[("makima", 87)] > kept[("pchip", 87)] > kept[("linear", 87)] and err["makima"] < err["pchip"] < err["linear"]
    xx = np.sort(rng.uniform(0, 10, 50)); assert np.allclose(makima(xx, 3 * xx + 1, xx[1:-1] + 0.0), 3 * xx[1:-1] + 1)
    print("methods OK")

def file_test(path):
    b = np.fromfile(path, np.uint8); n = b.size // 8; r = b[:n * 8].reshape(n, 8)
    ts = r[:, :4].copy().view("<u4").ravel().astype(np.int64); keep = ts < 0xFFFFFFF0; r = r[keep]; ts = ts[keep]
    lc = (r[:, 7] & 128) != 0
    def secs(x):
        d = np.diff(x); d[d < 0] += 2 ** 32; return np.concatenate([[0], np.cumsum(d)]) / 1e6
    tp, tl = secs(ts[~lc]), secs(ts[lc])
    d = int((ts[lc][0] - ts[~lc][0] + 2 ** 31) % 2 ** 32 - 2 ** 31)       # one time base, like loadRecords
    tp += max(-d, 0) / 1e6; tl += max(d, 0) / 1e6
    P = r[~lc, 4:6].copy().view("<i2").ravel() * 0.1875e-3 * 494.586
    q = r[lc].astype(float); raw = q[:, 4] + 256 * q[:, 5] + 65536 * q[:, 6]; raw[raw >= 2 ** 23] -= 2 ** 24
    F = raw * 1000 / 2 ** 31 * 1196.14 * 4.4482216152605
    lo, hi = max(tp[0], tl[0]), min(tp[-1], tl[-1]); k0, k1 = int(np.ceil(lo * 1000)), int(np.floor(hi * 1000))
    tg = np.arange(k0, k1 + 1) / 1000
    Pg, Fg = resample_gaps(tp, P, tg), resample_gaps(tl, F, tg)
    print("PT %d samples (%.0f /s), LC %d samples (%.0f /s); common span %.3f..%.3f s -> %d rows at 1 kHz" %
          (tp.size, tp.size / tp[-1], tl.size, tl.size / tl[-1], lo, hi, tg.size))
    print("NaN: pressure %d, thrust %d;  thrust max %.1f N (raw max %.1f N: the grid keeps the peak within %.2f %%)" %
          (np.isnan(Pg).sum(), np.isnan(Fg).sum(), np.nanmax(Fg), F.max(), 100 * (F.max() - np.nanmax(Fg)) / F.max()))
    m = ~((tl > 100) & (tl < 100.2)); Fh = resample_gaps(tl[m], F[m], tg)          # a hole of 0.2 s in the load cell data
    hole = (tg > 100.0) & (tg < 100.2); around = ((tg > 99) & (tg < 99.99)) | ((tg > 100.21) & (tg < 101))
    assert np.isnan(Fh[hole][5:-5]).all() and np.isfinite(Fh[around]).all()
    print("a gap of 0.2 s: NaN inside the gap, values around it - OK")

if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "methods": methods()
    elif len(sys.argv) >= 3 and sys.argv[1] == "file": file_test(sys.argv[2])
    else: sys.exit(__doc__)
