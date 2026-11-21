#!/usr/bin/env python3
"""Python copy of windowSpectrum and topPeaks (src/DAQ_Calibration_App.m, the Frequency slider of the motor window). No MATLAB here.

  spectrum_mirror.py synth            known resonances (87 Hz, 130 Hz) on a thrust ramp with timing jitter: frequency, amplitude, jitter effect
  spectrum_mirror.py file FILE.BIN T  load cell and PT spectrum of a real file in the 0.5 s window centred at T seconds
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resample_mirror import resample_gaps

def window_spectrum(t, y, fs, tc, win_s=0.5, df=0.25):
    """1:1 with windowSpectrum. A DFT taken directly at the real sample times (no interpolation: every interpolation loses amplitude
    near Nyquist), Hann window, mean and slope removed. Frequencies 0..Nyquist (fs / 2) in steps of df. Returns f, amp (single sided,
    unit of y), nyquist, message."""
    t = np.asarray(t, float).ravel(); y = np.asarray(y, float).ravel()
    if t.size < 20 or not np.isfinite(fs) or fs <= 0: return None, None, np.nan, "not enough data"
    ta, tb = tc - win_s / 2, tc + win_s / 2
    i1, i2 = np.searchsorted(t, ta, "left"), np.searchsorted(t, tb, "right")           # find(t >= ta, 1), find(t <= tb, 1, 'last')
    tw, yw = t[i1:i2], y[i1:i2]
    if tw.size < 3: return None, None, np.nan, "the end of the data is inside this window"
    if np.diff(tw).max() > max(0.05, 10 / fs): return None, None, np.nan, "a data gap is inside this window"
    if tw[0] - ta > 3 / fs or tb - tw[-1] > 3 / fs: return None, None, np.nan, "the end of the data is inside this window"
    pol = np.polyfit(tw - tc, yw, 1); yw = yw - np.polyval(pol, tw - tc)               # mean and slope out
    w = 0.5 - 0.5 * np.cos(2 * np.pi * (tw - ta) / win_s)                              # Hann over the window, at the real times
    f = np.arange(0, fs / 2 + df / 2, df); f = f[f <= fs / 2]
    X = np.exp(-2j * np.pi * np.outer(f, tw)) @ (w * yw)
    return f, 2 * np.abs(X) / w.sum(), fs / 2, ""

def top_peaks(f, amp, f_min=6.0, n_max=3, min_sep=6.0, rel=0.05):
    """1:1 with topPeaks: local maxima above f_min, at least rel x the largest, at least min_sep Hz apart, largest first."""
    k = np.nonzero(f >= f_min)[0]; a = amp[k]
    if a.size < 3: return []
    is_pk = np.concatenate([[False], (a[1:-1] > a[:-2]) & (a[1:-1] >= a[2:]), [False]])
    idx = np.nonzero(is_pk)[0]; idx = idx[np.argsort(-a[idx], kind="stable")]
    got = []
    for i in idx:
        if a[i] < rel * a.max(): break
        if all(abs(f[k[g]] - f[k[i]]) >= min_sep for g in got): got.append(i)
        if len(got) == n_max: break
    return [(f[k[i]], a[i]) for i in got]

def synth():
    rng = np.random.default_rng(5)
    fs = 321.0; n = int(fs * 10)
    t = np.sort(np.arange(n) / fs + rng.normal(0, 3e-4, n))                            # 0.3 ms of timing jitter
    sig = lambda x: 1500 * np.clip((x - 2) / 0.1, 0, 1) + 20 * np.sin(2 * np.pi * 87 * x) + 8 * np.sin(2 * np.pi * 130 * x)
    y = sig(t) + rng.normal(0, 2, n)
    f, amp, nyq, msg = window_spectrum(t, y, fs, 5.0)
    pk = top_peaks(f, amp)
    print("fs %.0f Hz, Nyquist %.1f Hz, frequencies %.2f Hz apart (window 0.5 s: true resolution about 2 Hz)" % (fs, nyq, f[1]))
    print("peaks:", ", ".join("%.1f Hz (%.2f N)" % p for p in pk))
    assert abs(pk[0][0] - 87) < 1 and abs(pk[1][0] - 130) < 1 and abs(pk[0][1] - 20) < 1.0 and abs(pk[1][1] - 8) < 0.8 and f[-1] <= nyq + 1e-9
    # a window with the same signal but with the samples taken as evenly spaced (what a plain FFT of the samples does)
    m = int(round(0.5 * fs)); i = np.searchsorted(t, 4.75); yy = y[i:i + m]; yy = yy - np.polyval(np.polyfit(np.arange(m), yy, 1), np.arange(m))
    w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(m) / (m - 1)); nfft = 4096
    naive = 2 * np.abs(np.fft.fft(yy * w, nfft)[:nfft // 2 + 1]) / w.sum(); fn = np.arange(nfft // 2 + 1) * fs / nfft
    far = lambda ff, aa: np.median(aa[(ff > 150) & (ff < 160)])
    print("noise floor at 150..160 Hz: direct DFT at the real times %.3f N, plain FFT that takes the samples as evenly spaced %.3f N" % (far(f, amp), far(fn, naive)))
    # a gap inside the window gives a message, not a wrong spectrum
    m2 = ~((t > 4.9) & (t < 5.1)); f2, a2, _, msg2 = window_spectrum(t[m2], y[m2], fs, 5.0); assert f2 is None and "gap" in msg2; print("gap:", msg2)
    f3, a3, _, msg3 = window_spectrum(t, y, fs, 9.95); assert f3 is None; print("end of data:", msg3)
    print("synth OK")

def read_file(path):
    b = np.fromfile(path, np.uint8); n = b.size // 8; r = b[:n * 8].reshape(n, 8)
    ts = r[:, :4].copy().view("<u4").ravel().astype(np.int64); keep = ts < 0xFFFFFFF0; r = r[keep]; ts = ts[keep]
    lc = (r[:, 7] & 128) != 0
    def secs(x):
        d = np.diff(x); d[d < 0] += 2 ** 32; return np.concatenate([[0], np.cumsum(d)]) / 1e6
    tp, tl = secs(ts[~lc]), secs(ts[lc]); d = int((ts[lc][0] - ts[~lc][0] + 2 ** 31) % 2 ** 32 - 2 ** 31)
    tp += max(-d, 0) / 1e6; tl += max(d, 0) / 1e6
    P = r[~lc, 4:6].copy().view("<i2").ravel() * 0.1875e-3 * 494.586
    q = r[lc].astype(float); raw = q[:, 4] + 256 * q[:, 5] + 65536 * q[:, 6]; raw[raw >= 2 ** 23] -= 2 ** 24
    return tp, P, tl, raw * 1000 / 2 ** 31 * 1196.14 * 4.4482216152605

def file_test(path, tc):
    tp, P, tl, F = read_file(path)
    for name, t, y, unit in (("load cell", tl, F, "N"), ("PT", tp, P, "psi")):
        fs = 1 / np.median(np.diff(t)); f, amp, nyq, msg = window_spectrum(t, y, fs, tc)
        if f is None: print(name, msg); continue
        pk = top_peaks(f, amp)
        print("%-9s fs %.1f Hz, Nyquist %.1f Hz; peaks: %s" % (name, fs, nyq, ", ".join("%.1f Hz (%.3g %s)" % (a, b, unit) for a, b in pk) or "-"))

if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "synth": synth()
    elif len(sys.argv) >= 4 and sys.argv[1] == "file": file_test(sys.argv[2], float(sys.argv[3]))
    else: sys.exit(__doc__)
