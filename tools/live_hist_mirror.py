#!/usr/bin/env python3
"""Python copy of the new parts of src/DAQ_Live_Monitor.m (no MATLAB here): history load (probe, load, finish) and the
burn analysis (burnStats), plus minMaxDecimate and insertGaps.

  live_hist_mirror.py burn                         burnStats on synthetic motor curves with a known impulse
  live_hist_mirror.py hist HOST PORT TOKEN         fake upload for 100 s; at 95 s load "last 30 s", then "all"; check times
"""
import math
import socket
import struct
import sys
import threading
import time

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import telem_mirror as tm  # noqa: E402

LC_MVV = 1000 / 2 ** 31
VPL = 0.0001875


# ------------------------------------------------------------------ burnStats (1:1 with the MATLAB)
def movmedian5(y):
    n = len(y); out = np.empty(n)
    for i in range(n):
        out[i] = np.median(y[max(0, i - 2):min(n, i + 3)])
    return out


def cross_time(ta, ya, tb, yb, thr):
    if yb == ya: return tb
    tc = ta + (thr - ya) / (yb - ya) * (tb - ta)
    return min(max(tc, min(ta, tb)), max(ta, tb))


def burn_stats(t, y, min_amp):
    r = dict(found=False, msg="")
    n = len(t)
    if n < 20: r["msg"] = "not enough data"; return r
    ys = movmedian5(y)
    dy = np.diff(y)
    sigma = 1.4826 * np.median(np.abs(dy)) / math.sqrt(2)
    if sigma == 0: sigma = np.std(dy, ddof=1) / math.sqrt(2)
    ip = int(np.argmax(ys)); pk = ys[ip]
    sigma = max(sigma, 1e-9 * max(1, abs(pk)))
    base0 = np.median(y)
    if pk <= base0: r["msg"] = "no peak"; return r
    thr = base0 + 0.05 * (pk - base0)
    below = np.nonzero(ys[:ip + 1] < thr)[0]
    i0 = 0 if len(below) == 0 else below[-1] + 1
    kb = (t >= t[i0] - 2.2) & (t < t[i0] - 0.2)
    base = np.median(y[kb]) if kb.sum() >= 5 else base0
    amp = pk - base
    r["strength"] = amp / sigma; r["base"] = base
    if amp < min_amp or r["strength"] < 15:
        r["msg"] = "no strong peak (%.3g, %.0f x)" % (amp, r["strength"]); return r
    thr = base + 0.05 * amp
    below = np.nonzero(ys[:ip + 1] < thr)[0]
    r["cutStart"] = len(below) == 0
    i0 = 0 if r["cutStart"] else below[-1] + 1
    after = np.nonzero(ys[ip:] < thr)[0]
    r["cutEnd"] = len(after) == 0
    i1 = n - 1 if r["cutEnd"] else ip + after[0] - 1
    if i1 - i0 < 2: r["msg"] = "too short"; return r
    yn = y - base; thrN = thr - base
    tt = list(t[i0:i1 + 1]); yy = list(yn[i0:i1 + 1])
    if not r["cutStart"]:
        tt.insert(0, cross_time(t[i0 - 1], ys[i0 - 1], t[i0], ys[i0], thr)); yy.insert(0, thrN)
    if not r["cutEnd"]:
        tt.append(cross_time(t[i1], ys[i1], t[i1 + 1], ys[i1 + 1], thr)); yy.append(thrN)
    tt = np.array(tt); yy = np.array(yy)
    r.update(tStart=tt[0], tEnd=tt[-1], burnS=tt[-1] - tt[0])
    srt = np.sort(yn[i0:i1 + 1])[::-1]
    r["top5"] = srt[:max(1, math.ceil(0.05 * len(srt)))].mean()
    r["integral"] = np.trapezoid(yy, tt)
    r["avg"] = r["integral"] / r["burnS"]
    r["tPeak"] = t[ip]; r["gapMax"] = np.diff(tt).max(); r["found"] = True
    return r


def motor_class(I):
    if I <= 2.5: return "A", 100 * I / 2.5
    k = math.ceil(math.log2(I / 2.5))
    return chr(ord("A") + k), 100 * I / (2.5 * 2 ** k)


def test_burn():
    rng = np.random.default_rng(1)
    fs = 64.0
    t = np.arange(0, 30, 1 / fs)
    # motor: 0.1 s ramp to 2000 N, regressive to 1500 N over 3 s, 0.2 s tail-off. Starts at t = 12 s.
    def curve(x):
        x = x - 12.0
        out = np.zeros_like(x)
        m = (x >= 0) & (x < 0.1); out[m] = 2000 * x[m] / 0.1
        m = (x >= 0.1) & (x < 3.1); out[m] = 2000 - 500 * (x[m] - 0.1) / 3
        m = (x >= 3.1) & (x < 3.3); out[m] = 1500 * (1 - (x[m] - 3.1) / 0.2)
        return out
    xf = np.linspace(0, 30, 3_000_001)
    I_true = np.trapezoid(curve(xf), xf)
    offset = 37.0                                  # an untared load cell (N)
    y = curve(t) + offset + rng.normal(0, 3, t.size)
    y[500] += 400                                  # one wrong sample before the burn: not the peak
    r = burn_stats(t, y, 5)
    cls, pct = motor_class(r["integral"])
    print("true impulse %.1f N s, burn ~3.3 s" % I_true)
    print("found=%s start=%.3f end=%.3f burn=%.3f top5=%.1f avg=%.1f I=%.1f (%.2f %% err) class %s %.0f%% base=%.2f strength=%.0f"
          % (r["found"], r["tStart"], r["tEnd"], r["burnS"], r["top5"], r["avg"], r["integral"],
             100 * (r["integral"] - I_true) / I_true, cls, pct, r["base"], r["strength"]))
    assert r["found"] and abs(r["tStart"] - 12.005) < 0.02 and abs(r["base"] - offset) < 1.5
    assert abs(r["integral"] - I_true) / I_true < 0.01
    assert 1950 < r["top5"] < 2010 and abs(r["avg"] - r["integral"] / r["burnS"]) < 1e-9
    assert motor_class(640) == ("I", 100.0) and motor_class(641)[0] == "J" and motor_class(5.0)[0] == "B"
    # noise only: no burn
    r2 = burn_stats(t, offset + rng.normal(0, 3, t.size), 5)
    print("noise only:", r2["msg"]); assert not r2["found"]
    # a tap of 20 N for 0.1 s on noise of 3 N: below 15 x noise? (20 / 3 ~ 7): no burn
    yt = offset + rng.normal(0, 3, t.size); yt[(t > 5) & (t < 5.1)] += 20
    r3 = burn_stats(t, yt, 5); print("tap:", r3["msg"]); assert not r3["found"]
    # data starts inside the burn
    k = t >= 13
    r4 = burn_stats(t[k], y[k], 5); print("cut start:", r4["found"], r4.get("cutStart")); assert r4["cutStart"]
    print("burn OK")


def test_plot_helpers():
    def insert_gaps(t, v, thr):
        g = np.nonzero(np.diff(t) > thr)[0]
        if len(g) == 0: return t, v
        k = np.zeros(len(t)); k[g + 1] = 1
        pos = np.arange(len(t)) + np.cumsum(k).astype(int)
        t2 = np.zeros(len(t) + len(g)); v2 = np.full(len(t2), np.nan)
        t2[pos] = t; v2[pos] = v; t2[pos[g] + 1] = t[g]
        return t2, v2
    t = np.array([0, 1, 2, 10, 11, 30.0]); v = np.arange(6.0)
    t2, v2 = insert_gaps(t, v, 0.5 * 4)
    assert list(t2) == [0, 1, 2, 2, 10, 11, 11, 30] and np.isnan(v2[[3, 6]]).all() and v2[4] == 3, (t2, v2)
    print("insertGaps OK")


# ------------------------------------------------------------------ history load (1:1 with the MATLAB TCP path)
class Hist:
    def __init__(self, host, port, tok, hist_s):
        self.s = socket.create_connection((host, port), 5)
        tm.hello(self.s, 2, tok)
        self.rx = b""; self.t0 = time.monotonic(); self.hist_s = hist_s
        self.buf = {}                       # d -> list of (raw, v, sn, ord)
        self.ser = {}                       # (d, sn) -> (t list, v list)
        self.ord = 0; self.state = "start"; self.age = None; self.full = False

    def frames(self):
        while True:
            st, typ, pay, nxt = tm.tp_next(self.rx, 0, True)
            if st == 0:
                self.rx += self.s.recv(65536); continue
            self.rx = self.rx[nxt:]
            if st == 1: return typ, pay

    def get(self, cursor):
        self.s.sendall(tm.frame(tm.GET, struct.pack("<QHH", cursor, 15000, 2000)))

    def run(self):
        typ, pay = self.frames(); assert typ == tm.ACK
        head = struct.unpack("<Q", pay)[0]
        if math.isinf(self.hist_s) or head <= 300:
            self.cursor = 0; self.load_to = head; self.state = "loading"
        else:
            self.load_to = head; self.cursor = head - 300; self.state = "probe"
        self.get(self.cursor)
        nb = 0
        while self.state != "live":
            typ, pay = self.frames(); assert typ == tm.BATCH
            self.batch(pay); nb += 1
            if self.state != "live": self.get(self.cursor)
        self.s.close()
        return nb

    def batch(self, pay):
        t_arr = time.monotonic() - self.t0
        nxt, age = struct.unpack_from("<QI", pay)
        self.age = None if age == 0xFFFFFFFF else age
        full = len(pay) > 7500
        us_all = {}; nframes = 0; pos = 12
        while pos < len(pay):
            st, typ, p, pos2 = tm.tp_next(pay, pos, False)
            if st != 1: break
            pos = pos2; nframes += 1
            src = pay[pos - len(p) - 11 + 3]
            if typ not in (tm.DATA_PT, tm.DATA_LC) or src >= 8: continue
            n = len(p) // 8
            recs = [struct.unpack_from("<IhH", p, 8 * i) for i in range(n)]
            out = []
            for us, rd, sq in recs:
                if us >= tm.META_BASE: continue
                if typ == tm.DATA_PT and not (sq & 0x8000): out.append((us, rd * VPL, 1))
                if typ == tm.DATA_LC and (sq & 0x8000):
                    c = (rd & 0xFFFF) + 65536 * (sq & 0xFF)
                    if c >= 2 ** 23: c -= 2 ** 24
                    out.append((us, c * LC_MVV, 2))
            if self.state == "loading":
                self.ord += 1
                self.buf.setdefault(src, []).extend((u, v, sn, self.ord) for u, v, sn in out)
            else:
                us_all.setdefault(src, []).extend(u for u, _, _ in out)
        if self.state == "probe":
            span = 0
            for raw in us_all.values():
                if len(raw) < 2: continue
                w = [((r - raw[0] + 2 ** 31) % 2 ** 32) - 2 ** 31 for r in raw]
                span = max(span, (max(w) - min(w)) / 1e6)
            c0 = max(0, self.load_to - math.ceil(nframes / span * self.hist_s * 1.05)) if span >= 1 and nframes else 0
            print("  probe: %d frames span %.2f s -> %.2f frames/s, start cursor %d (head %d)"
                  % (nframes, span, nframes / max(span, 1e-9), c0, self.load_to))
            self.cursor = c0; self.state = "loading"; self.load_from = c0
            return
        self.cursor = nxt
        if not full: self.finish(t_arr)

    def finish(self, t_arr):
        U = {}; last_ord = {}
        for d, rows in self.buf.items():
            raw = np.array([r[0] for r in rows], dtype=float)
            du = ((np.diff(raw) + 2 ** 31) % 2 ** 32) - 2 ** 31
            du[np.abs(du) > 30e6] = 1e5
            U[d] = np.concatenate([[0], np.cumsum(du)])
            last_ord[d] = rows[-1][3]
        ref = max(last_ord, key=last_ord.get)
        target = t_arr - (self.age or 0) / 1000
        off = {ref: target - U[ref][-1] / 1e6}
        ords = np.array([r[3] for r in self.buf[ref]], dtype=float)
        fo, ia_rev = np.unique(ords[::-1], return_index=True)
        ia = len(ords) - 1 - ia_rev
        ft = U[ref][ia] / 1e6 + off[ref]
        for d in self.buf:
            if d == ref: continue
            o = last_ord[d]
            tr = np.interp(o, fo, ft) if fo[0] <= o <= fo[-1] and len(fo) > 1 else (ft[0] if o <= fo[0] else ft[-1])
            off[d] = tr - U[d][-1] / 1e6
        for d, rows in self.buf.items():
            t = U[d] / 1e6 + off[d]
            for sn in (1, 2):
                k = [i for i, r in enumerate(rows) if r[2] == sn]
                if k: self.ser[(d, sn)] = (t[k], np.array([rows[i][1] for i in k]), np.array([rows[i][0] for i in k]))
        self.state = "live"


def test_hist(host, port, tok):
    sent = {}
    up = threading.Thread(target=tm.uploader, args=(host, port, tok, 100, sent), daemon=True)
    t_up = time.monotonic(); up.start()
    time.sleep(95)
    for hs in (30.0, math.inf):
        h = Hist(host, port, tok, hs)
        t_c = time.monotonic()
        nb = h.run()
        t_load = time.monotonic() - t_c
        print("history %s: %d batches in %.2f s" % (hs, nb, t_load))
        # fake time of each sample (s since upload start) vs its monitor time + the time of Connect
        offs = {}
        for (d, sn), (t, v, raw) in sorted(h.ser.items()):
            fake = {u: ft for u, _, ft in sent[d]["pt" if sn == 1 else "lc"]}
            ft = np.array([fake[u] for u in raw])
            o = (t + (h.t0 - t_up)) - ft                  # should be about constant: the delay of the link
            offs[(d, sn)] = o
            print("  DAQ %d %s: %d samples, fake time %.2f..%.2f s (%.1f s of data), offset %.3f..%.3f s"
                  % (d, "PT" if sn == 1 else "LC", len(t), ft[0], ft[-1], ft[-1] - ft[0], o.min(), o.max()))
            assert o.max() - o.min() < 0.02, "board clock unwrap (DAQ 5 wraps at 3 s) is not smooth"
            assert np.all(np.diff(t) > 0)
            if math.isinf(hs):
                assert ft[0] < 0.6, "All: data must start at the start of the upload"
            else:
                assert 25 < ft[-1] - ft[0] < 36, "last 30 s: about 30 s of data"
        allo = np.concatenate(list(offs.values()))
        print("  all DAQs: offset spread %.3f s (must be < 0.6 s: two clocks line up)" % (allo.max() - allo.min()))
        assert allo.max() - allo.min() < 0.6 and abs(np.median(allo)) < 1.5
    print("hist OK")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "burn":
        test_plot_helpers(); test_burn()
    elif len(sys.argv) >= 5 and sys.argv[1] == "hist":
        test_hist(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    else:
        print(__doc__)
