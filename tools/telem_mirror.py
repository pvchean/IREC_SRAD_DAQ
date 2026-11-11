#!/usr/bin/env python3
"""Fake upload DAQ (with two source DAQs, as the chain gives them), and a line-for-line Python copy of the TCP decode of
src/DAQ_Live_Monitor.m (no MATLAB here).

  telem_mirror.py check  HOST PORT TOKEN [SECONDS]   upload fake data and download it with the mirror; assert the values
  telem_mirror.py upload HOST PORT TOKEN [SECONDS]   only the fake upload DAQ (to test the real MATLAB monitor)

DAQ 5: PT 5 Hz sine 1..4 V at 60 Hz, load cell sine +/-300000 counts at 64 Hz (negative counts test the i24 sign); its clock
wraps (u32) 3 s into the test. DAQ 6: PT only, 2 Hz sine, own clock. CAL block of each at the start and every 5 s. STATUS (ID 3) every 2 s.
"""
import math
import socket
import struct
import sys
import threading
import time
import zlib

SYNC = b"\xA5\x5B"
HELLO, ACK, DATA_PT, DATA_LC, CAL, STATUS, GET, BATCH, NAK = range(1, 10)
META_BASE = 0xFFFFFFF0
CAL_A = (2500.0, -12.5, 98.25, 0.5)        # pt_a, pt_b, lc_a, lc_b
CAL_REV = 42


def frame(t, pay, src=0):
    body = struct.pack("<BBBH", 1, src, t, len(pay)) + pay
    return SYNC + body + struct.pack("<I", zlib.crc32(body))


def hello(sock, role, tok):
    sock.sendall(frame(HELLO, bytes([role]) + tok.encode().ljust(16, b"\0") + bytes(8)))


# ---------------------------------------------------------------- fake upload DAQ
def rec_ts(us):
    return META_BASE - 1 if us >= META_BASE else us      # src/record.h: a sensor time is never a META time


def pt_rec(us, volts, seq):
    return struct.pack("<IhH", us, int(round(volts / 0.0001875)), seq & 0x7FFF)


def lc_rec(us, count, seq):
    c = count & 0xFFFFFF
    return struct.pack("<IHH", us, c & 0xFFFF, (c >> 16) | 0x8000 | ((seq & 0x7F) << 8))


# Fake chain: the upload DAQ (ID 3, no sensors) sends the data of DAQ 5 (PT + load cell) and DAQ 6 (PT only).
# Each board has its own clock: DAQ 5 wraps 3 s into the test, DAQ 6 starts at an arbitrary time.
DEVS = {5: dict(clk0=(1 << 32) - 3_000_000, pt=True, lc=True, cal=CAL_A, rev=42, f0=5.0),
        6: dict(clk0=123_456_789, pt=True, lc=False, cal=(1800.0, 3.5, 1.0, 0.0), rev=7, f0=2.0)}
SELF = 3


def cal_block(rev=CAL_REV, vals=CAL_A, flags=3):
    pay = [(flags << 24) | rev] + [struct.unpack("<I", struct.pack("<f", v))[0] for v in vals]
    crc = zlib.crc32(struct.pack("<5I", *pay))
    return b"".join(struct.pack("<II", META_BASE + k + 1, p) for k, p in enumerate(pay + [crc]))


def pt_volts(t, f0=5.0):
    return 2.5 + 1.5 * math.sin(2 * math.pi * f0 * t)


def lc_count(t):
    return int(round(300000 * math.sin(2 * math.pi * 0.7 * t)))


def uploader(host, port, tok, seconds, sent):
    s = socket.create_connection((host, port), 5)
    hello(s, 1, tok)
    assert s.recv(64)[4] == ACK
    t_start = time.monotonic()
    st = {d: dict(nxt_pt=0.0, nxt_lc=0.0, seq_pt=0, seq_lc=0, last_cal=-1e9) for d in DEVS}
    for d in DEVS:
        sent[d] = {"pt": [], "lc": []}
    last_st = -1e9
    while time.monotonic() - t_start < seconds:
        now = time.monotonic() - t_start
        out = b""
        for d, cfg in DEVS.items():
            x = st[d]
            pt, lc = [], []
            if now - x["last_cal"] >= 5:
                out += frame(CAL, cal_block(cfg["rev"], cfg["cal"], 3 if cfg["lc"] else 1), d); x["last_cal"] = now
            while x["nxt_pt"] <= now:
                us = rec_ts((cfg["clk0"] + int(x["nxt_pt"] * 1e6)) & 0xFFFFFFFF)
                pt.append(pt_rec(us, pt_volts(x["nxt_pt"], cfg["f0"]), x["seq_pt"]))
                sent[d]["pt"].append((us, pt_volts(x["nxt_pt"], cfg["f0"]), x["nxt_pt"]))
                x["seq_pt"] += 15; x["nxt_pt"] += 1 / 60
            while cfg["lc"] and x["nxt_lc"] <= now:
                us = rec_ts((cfg["clk0"] + int(x["nxt_lc"] * 1e6)) & 0xFFFFFFFF)
                lc.append(lc_rec(us, lc_count(x["nxt_lc"]), x["seq_lc"]))
                sent[d]["lc"].append((us, lc_count(x["nxt_lc"]), x["nxt_lc"]))
                x["seq_lc"] += 5; x["nxt_lc"] += 1 / 64
            if pt: out += frame(DATA_PT, b"".join(pt), d)
            if lc: out += frame(DATA_LC, b"".join(lc), d)
        if now - last_st >= 2:
            out += frame(STATUS, struct.pack("<bBIIIH", -61, 1, 3, 17, int(now), 0), SELF); last_st = now
        s.sendall(out)
        time.sleep(0.5)
    s.close()


# ---------------------------------------------------------------- mirror of DAQ_Live_Monitor.m (TCP source)
def tp_next(buf, pos, check_crc):
    """Returns (st, type, pay, nxt) as tpNext (0-based here)."""
    n = len(buf) - pos
    if n < 2: return 0, 0, b"", pos
    if buf[pos] != 0xA5 or buf[pos + 1] != 0x5B:
        k = buf.find(b"\xA5", pos + 1)
        return -1, 0, b"", (len(buf) if k < 0 else k)
    if n < 7: return 0, 0, b"", pos
    ln = buf[pos + 5] | buf[pos + 6] << 8
    if buf[pos + 2] != 1 or ln > 16000: return -1, 0, b"", pos + 1
    if n < 11 + ln: return 0, 0, b"", pos
    if check_crc and zlib.crc32(buf[pos + 2:pos + 7 + ln]) != struct.unpack_from("<I", buf, pos + 7 + ln)[0]:
        return -1, 0, b"", pos + 1
    return 1, buf[pos + 4], buf[pos + 7:pos + 7 + ln], pos + 11 + ln


def decode_recs(p):
    n = len(p) // 8
    return [struct.unpack_from("<IhH", p, 8 * i) for i in range(n)]


def lc_count_dec(rd, sq):
    c = (rd % 65536) + 65536 * (sq & 255)
    return c - 2**24 if c >= 2**23 else c


def cal_block_dec(p):
    r = decode_recs(p)
    if len(r) < 6: return None
    kind = [u - 4294967280 for u, _, _ in r]
    pay = [(rd % 65536) + 65536 * sq for _, rd, sq in r]
    for i in range(len(r) - 6, -1, -1):
        if kind[i:i + 6] != [1, 2, 3, 4, 5, 6]: continue
        if zlib.crc32(struct.pack("<5I", *pay[i:i + 5])) != pay[i + 5]: continue
        flags = pay[i] // 2**24
        f = struct.unpack("<4f", struct.pack("<4I", *pay[i + 1:i + 5]))
        return dict(rev=pay[i] % 2**24, ptSet=bool(flags & 1), lcSet=bool(flags & 2), f=f)
    return None


class Map:
    def __init__(self): self.have = False; self.rawRef = 0; self.uRef = 0; self.off = 0.0

    def times(self, raw, tArr, ageMs, catching):
        if not raw: return []
        if not self.have: self.rawRef = raw[-1]; self.uRef = 0
        u = [self.uRef + ((x - self.rawRef + 2**31) % 2**32) - 2**31 for x in raw]
        iNew = max(range(len(u)), key=lambda i: u[i]); uNew = u[iNew]
        target = tArr if ageMs is None else tArr - ageMs / 1000
        err = target - (uNew / 1e6 + self.off)
        if not self.have or abs(err) > 2:
            if not self.have or not catching: self.off = target - uNew / 1e6; self.have = True
        elif not catching:
            self.off += 0.05 * err
        self.rawRef = raw[iNew]; self.uRef = uNew
        return [x / 1e6 + self.off for x in u]


def mirror(host, port, tok, seconds, got):
    s = socket.create_connection((host, port), 5)
    s.settimeout(0.1)
    hello(s, 2, tok)
    rx = b""; state = "hello"; cursor = 0xFFFFFFFFFFFFFFFF; t0 = time.monotonic(); maps = {}
    while time.monotonic() - t0 < seconds:
        try:
            d = s.recv(65536)
            if not d: break
            rx += d
        except socket.timeout:
            pass
        pos = 0
        while True:
            st, typ, pay, nxt = tp_next(rx, pos, True)
            if st == 0: break
            pos = nxt
            if st < 0: got["junk"] += 1; continue
            if state == "hello":
                assert typ == ACK and len(pay) == 8, (typ, pay)
                state = "run"
            else:
                assert typ == BATCH
                tArr = time.monotonic() - t0
                cursor, age = struct.unpack_from("<QI", pay)
                ageMs = None if age == 0xFFFFFFFF else age
                per = {}                                   # src -> dict(us_pt, v_pt, us_lc, c_lc)
                p0 = 12
                while p0 < len(pay):
                    st2, t2, p, n2 = tp_next(pay, p0, False)
                    if st2 != 1: got["badinner"] += 1; break
                    src = pay[p0 + 3]
                    p0 = n2
                    if t2 == STATUS and len(p) == 16:
                        got["status"] = (src,) + struct.unpack("<bBIIIH", p)
                        continue
                    if src > 7: continue
                    e = per.setdefault(src, dict(us_pt=[], v_pt=[], us_lc=[], c_lc=[]))
                    if t2 == DATA_PT:
                        for u, rd, sq in decode_recs(p):
                            if u < META_BASE and not sq & 0x8000: e["us_pt"].append(u); e["v_pt"].append(rd * 0.0001875)
                    elif t2 == DATA_LC:
                        for u, rd, sq in decode_recs(p):
                            if u < META_BASE and sq & 0x8000: e["us_lc"].append(u); e["c_lc"].append(lc_count_dec(rd, sq))
                    elif t2 == CAL:
                        c = cal_block_dec(p)
                        if c: got["cal"][src] = c
                for src, e in per.items():
                    m = maps.setdefault(src, Map())
                    t = m.times(e["us_pt"] + e["us_lc"], tArr, ageMs, len(pay) > 7500)
                    n = len(e["us_pt"])
                    got["pt"].setdefault(src, []).extend(zip(e["us_pt"], e["v_pt"], t[:n]))
                    got["lc"].setdefault(src, []).extend(zip(e["us_lc"], e["c_lc"], t[n:]))
                got["ages"].append(ageMs)
                got["batches"] += 1
            if state == "run":
                s.sendall(frame(GET, struct.pack("<QHH", cursor, 15000, 2000)))
        rx = rx[pos:]
    s.close()


def main():
    if len(sys.argv) < 5: print(__doc__); return 2
    mode, host, port, tok = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    secs = float(sys.argv[5]) if len(sys.argv) > 5 else 8
    sent = {}
    if mode == "upload":
        uploader(host, port, tok, secs, sent)
        print("sent " + ", ".join(f"DAQ {d}: {len(v['pt'])} PT {len(v['lc'])} LC" for d, v in sent.items()))
        return 0
    got = {"pt": {}, "lc": {}, "cal": {}, "status": None, "ages": [], "batches": 0, "junk": 0, "badinner": 0}
    th = threading.Thread(target=mirror, args=(host, port, tok, secs + 2, got))
    th.start(); time.sleep(0.5)
    uploader(host, port, tok, secs, sent)
    th.join()

    ok = True
    def check(cond, msg):
        nonlocal ok
        print(("OK   " if cond else "FAIL ") + msg); ok &= bool(cond)

    seen = lambda k: sorted(d for d, v in got[k].items() if v)
    check(seen("pt") == [5, 6] and seen("lc") == [5], f"data seen from DAQ {seen('pt')} (PT), {seen('lc')} (LC); none from ID 3")
    mean_off = {}
    for d in DEVS:
        sp = {u: v for u, v, _ in sent[d]["pt"]}; sl = {u: v for u, v, _ in sent[d]["lc"]}
        gp = got["pt"].get(d, []); gl = got["lc"].get(d, [])
        check(len(gp) == len(sp) and len(gl) == len(sl), f"DAQ {d}: all records came back, PT {len(gp)}/{len(sp)}, LC {len(gl)}/{len(sl)}")
        check(all(abs(v - sp[u]) <= 0.0001875 / 2 + 1e-12 for u, v, _ in gp), f"DAQ {d}: PT volts match (within 1/2 LSB)")
        check(all(c == sl[u] for u, c, _ in gl), f"DAQ {d}: LC counts match exactly")
        for k, g, exp in (("pt", gp, 1 / 60), ("lc", gl, 1 / 64)):
            if not g: continue
            t = [x[2] for x in g]; dd = [b - a for a, b in zip(t, t[1:])]
            check(min(dd) > 0 and len([x for x in dd if abs(x - exp) > 0.003]) <= 2,
                  f"DAQ {d}: {k.upper()} times go up, step {exp*1e3:.2f} ms +/-3 ms (min {min(dd)*1e3:.3f} ms)")
        tt = {u: tr for u, _, tr in sent[d]["pt"]}
        diffs = [t - tt[u] for u, _, t in gp]
        mean_off[d] = sum(diffs) / len(diffs)
        check(max(diffs) - min(diffs) < 0.6, f"DAQ {d}: mapped time follows the true time within {max(diffs)-min(diffs):.3f} s")
        c = got["cal"].get(d)
        check(c is not None and c["rev"] == DEVS[d]["rev"] and c["ptSet"] and c["lcSet"] == DEVS[d]["lc"] and
              all(abs(a - b) < 1e-6 for a, b in zip(c["f"], DEVS[d]["cal"])), f"DAQ {d}: its own calibration block decoded: {c}")
    check(abs(mean_off[5] - mean_off[6]) < 0.7, f"the two DAQs line up: {abs(mean_off[5]-mean_off[6]):.3f} s apart")
    check(any(c < 0 for _, c, _ in got["lc"][5]), "negative LC counts were in the test")
    pt5 = [u for u, _, _ in got["pt"][5]]
    check(any(u < 3_000_000 for u in pt5) and any(u > 2**32 - 3_000_000 for u in pt5), "the clock of DAQ 5 wrapped during the test")
    check(got["status"] is not None and got["status"][:2] == (SELF, -61), f"status of the upload DAQ (ID 3): {got['status']}")
    check(got["junk"] == 0 and got["badinner"] == 0, "no junk bytes, no bad inner frames")
    ages = [a for a in got["ages"] if a is not None]
    print(f"     {got['batches']} batches, data age max {max(ages) if ages else '-'} ms")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
