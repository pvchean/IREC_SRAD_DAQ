#!/usr/bin/env python3
"""Python copy of badTimeMask() + secondsOf() of src/DAQ_Calibration_App.m, and a check of the CRC-32 table loop.

  time_clean_mirror.py FILE.BIN [...]     for each sensor: records dropped, span before and after the clean-up
  time_clean_mirror.py crc                the table loop of crc32Of equals zlib.crc32 on random data

A sensor time only goes forward (mod 2^32). A step that is not small cuts the records of a sensor in segments. A segment of
fewer than MIN_SEG records is damage (for example log text that a bad download put in a file), a long segment is data.
Change this file and badTimeMask in the app together.
"""
import sys
import zlib

import numpy as np

MIN_SEG = 50


def bad_time_mask(ts):
    """ts: uint32 times of one sensor, in file order. Return (keep mask, number of dropped records).
    Pass 1: a step that is not small cuts the records in segments; a segment of fewer than MIN_SEG records is dropped.
    Pass 2: the time of a sensor never goes back. At a step back, the record that is out of order is dropped (this is
    the record that a lost piece of a download joined to the wrong bytes: its time is wrong by up to 65 ms)."""
    n = len(ts)
    keep = np.ones(n, bool)
    if n < 3:
        return keep, 0
    d = np.diff(ts.astype(np.int64)) % (1 << 32)           # forward step, with the wrap of the clock
    tol = max(1e5, 500 * float(np.median(d)))
    cut = np.flatnonzero(d > tol)
    if cut.size:
        start = np.concatenate(([0], cut + 1))
        end = np.concatenate((cut, [n - 1]))
        short = (end - start + 1) < MIN_SEG
        if not short.all():                                # all short: no long segment to compare with
            for a, b in zip(start[short], end[short]):
                keep[a:b + 1] = False
    for _ in range(50):
        idx = np.flatnonzero(keep)
        s = (np.diff(ts[idx].astype(np.int64)) + (1 << 31)) % (1 << 32) - (1 << 31)    # signed step
        back = np.flatnonzero(s < 0)                       # step between kept record j and j + 1
        if back.size == 0:
            break
        u = np.concatenate(([0], np.cumsum(s)))
        drop = back + 1                                    # the later record is the bad one ...
        prev_ok = back > 0
        late = np.zeros(back.size, bool)
        late[prev_ok] = u[back[prev_ok] + 1] >= u[back[prev_ok] - 1]
        drop[late] = back[late]                            # ... unless the next one still follows the one before
        keep[idx[np.unique(drop)]] = False
    return keep, int((~keep).sum())


def seconds_of(ts):
    d = np.diff(ts.astype(np.int64)) % (1 << 32)
    return np.concatenate(([0], np.cumsum(d))) / 1e6


def crc_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (0xEDB88320 ^ (c >> 1)) if c & 1 else c >> 1
        t.append(c)
    return t


def crc_loop(data):
    t, c = crc_table(), 0xFFFFFFFF
    for b in data:
        c = t[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def load(path):
    d = open(path, "rb").read()
    n = len(d) // 8
    a = np.frombuffer(d[:n * 8], dtype=np.uint8).reshape(n, 8)
    ts = a[:, :4].copy().view("<u4").ravel()
    meta = ts >= 0xFFFFFFF0
    a, ts = a[~meta], ts[~meta]
    lc = (a[:, 7] & 0x80) != 0
    return ts, lc


def main():
    if sys.argv[1:] == ["crc"]:
        rng = np.random.default_rng(1)
        for n in (0, 1, 3, 20, 16384):
            b = rng.integers(0, 256, n, dtype=np.uint8).tobytes()
            print(n, crc_loop(b) == zlib.crc32(b))
        print("known:", crc_loop(bytes([1, 2, 3])) == 1438416925)
        return
    for p in sys.argv[1:]:
        ts, lc = load(p)
        for name, m in (("PT", ~lc), ("LC", lc)):
            t = ts[m]
            if not len(t):
                continue
            keep, nd = bad_time_mask(t)
            before = seconds_of(t)[-1]
            after = seconds_of(t[keep])[-1]
            print(f"{p.split('/')[-1]:22s} {name}: {len(t)} records, dropped {nd}, span {before:.1f} s -> {after:.3f} s")


if __name__ == "__main__":
    main()
