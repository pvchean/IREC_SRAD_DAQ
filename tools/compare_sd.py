#!/usr/bin/env python3
"""Compare two DAQ record files (8-byte records, see README.md).

  compare_sd.py A.BIN [B.BIN]

One file: decode it and report the records, the seq steps, the time, and the readings.
Two files: also find where B starts inside A (or A inside B) and compare the records one by one.
The typical test: A = F<n>D3.BIN of board 3 (own data), B = F<m>D3.BIN of board 5 (the same data over UART).
The seq field is 16 bit and wraps every 76 s, so the files are matched by their bytes, not by seq.
"""
import struct
import sys

REC = 8


def load(path):
    data = open(path, "rb").read()
    n = len(data) // REC
    recs = [data[i * REC:(i + 1) * REC] for i in range(n)]
    return recs, len(data) % REC


def describe(name, recs, tail):
    print(f"{name}: {len(recs)} records, {tail} stray bytes")
    if not recs:
        return
    ts, rd, sq = zip(*(struct.unpack("<IhH", r) for r in recs))
    breaks = sum(1 for a, b in zip(sq, sq[1:]) if (b - a) & 0xFFFF != 1)
    missing = sum(((b - a) & 0xFFFF) - 1 for a, b in zip(sq, sq[1:]) if (b - a) & 0xFFFF != 1)
    back = sum(1 for a, b in zip(ts, ts[1:]) if (b - a) & 0xFFFFFFFF > 0x80000000)
    dts = [(b - a) & 0xFFFFFFFF for a, b in zip(ts, ts[1:])]
    near = [d for d in dts if d < 10000]
    print(f"  seq {sq[0]}..{sq[-1]}  seq_breaks={breaks} missing={missing}  time_back={back}")
    if near:
        print(f"  mean interval {sum(near) / len(near):.0f} us, max gap {max(dts)} us, "
              f"span {(ts[-1] - ts[0]) & 0xFFFFFFFF} us")
    print(f"  readings min={min(rd)} max={max(rd)} mean={sum(rd) / len(rd):.1f}")


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    a, ta = load(sys.argv[1])
    describe(sys.argv[1], a, ta)
    if len(sys.argv) == 2:
        return
    b, tb = load(sys.argv[2])
    describe(sys.argv[2], b, tb)
    if not a or not b:
        sys.exit("one file is empty")
    # Find the start of the shorter-starting file inside the other one.
    if b[0] in set(a):
        first, second, label = a, b, "B starts inside A"
    elif a[0] in set(b):
        first, second, label = b, a, "A starts inside B"
    else:
        sys.exit("NO MATCH: the first record of neither file is in the other file")
    off = first.index(second[0])
    n = min(len(second), len(first) - off)
    bad = [i for i in range(n) if first[off + i] != second[i]]
    print(f"{label}: offset {off} records ({off} records before the common part)")
    print(f"common records compared: {n}, different: {len(bad)}")
    if bad:
        i = bad[0]
        print(f"  first difference at common record {i}: {first[off + i].hex()} vs {second[i].hex()}")
    rest_first, rest_second = len(first) - off - n, len(second) - n
    print(f"records after the common part: first file {rest_first}, second file {rest_second}")
    print("RESULT:", "IDENTICAL in the common part" if not bad else "DIFFERENT")


main()
