#!/usr/bin/env python3
"""Simple check of DAQ record files (8-byte records, see README.md).

  compare_sd.py A.BIN [B.BIN]

Prints for each file: the number of records, the time, and min / mean / max of the readings.
With two files it also says if they hold the same data.
Example: A = local file of board 3 (F<n>D3.BIN), B = copy of it on the SD card of board 5.
"""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    return [struct.unpack("<IhH", d[i:i + 8]) for i in range(0, len(d) - 7, 8)], [d[i:i + 8] for i in range(0, len(d) - 7, 8)]


def stats(path, recs):
    if not recs:
        print(f"{path}: EMPTY")
        return None
    rd = [r[1] for r in recs]
    secs = ((recs[-1][0] - recs[0][0]) & 0xFFFFFFFF) / 1e6
    lost = sum(((b[2] - a[2]) & 0xFFFF) - 1 for a, b in zip(recs, recs[1:]) if (b[2] - a[2]) & 0xFFFF != 1)
    print(f"{path}\n  {len(recs)} records, {secs:.1f} s, lost samples {lost}, "
          f"readings min {min(rd)} / mean {sum(rd) / len(rd):.1f} / max {max(rd)}")
    return len(recs), min(rd), sum(rd) / len(rd), max(rd)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    a, ab = load(sys.argv[1])
    sa = stats(sys.argv[1], a)
    if len(sys.argv) == 2:
        return
    b, bb = load(sys.argv[2])
    sb = stats(sys.argv[2], b)
    if not sa or not sb:
        sys.exit("RESULT: one file is empty")
    # Exact test: the shorter-starting file is a piece of the other one (the remote copy starts later).
    x, y = (ab, bb) if bb[0] in set(ab) else (bb, ab)
    if y[0] in set(x):
        off = x.index(y[0])
        n = min(len(y), len(x) - off)
        diff = sum(1 for i in range(n) if x[off + i] != y[i])
        print(f"\n{n} records in common, {diff} different")
        if diff == 0 and n > 0:
            print("RESULT: SAME DATA")
            return
    # Broad test: similar size of the data and similar readings.
    near = abs(sa[2] - sb[2]) <= 5 and abs(sa[1] - sb[1]) <= 20 and abs(sa[3] - sb[3]) <= 20
    print("RESULT:", "BROADLY THE SAME (readings are alike, records are not identical)" if near else "DIFFERENT")


main()
