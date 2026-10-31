#!/usr/bin/env python3
"""Statistics of a record file (PFS file or SD file) by sensor. The layout is in src/record.h.

  rec_stats.py FILE            records, span, rate, lost samples, gaps and readings of each sensor
  rec_stats.py FILE1 FILE2     the same for both, and for each sensor: is one record list a contiguous piece of the other?

META records (time field 0xFFFFFFF0 or more) are not samples. A calibration block is 6 of them (kinds 1 to 6, CRC-32 in kind 6).
The tool leaves them out of the sensors, counts them, and prints the last valid block and the number of valid blocks.

A file is PT-only when the top seq bit rarely changes (flips * 16384 < n). This also covers the files of the firmware
before the load cell, which use all 16 bits for the PT seq. PT seq is mod 32768, load cell seq is mod 128.
The seq counts also the samples that were not recorded, so a pause of REC shows as lost samples (and as a long gap).
"""
import struct
import sys
import zlib

SENSORS = ("PT", "LC")
SEQ_MOD = {"PT": 32768, "LC": 128}
META_BASE = 0xFFFFFFF0


def decode_cal(metas):
    """metas: list of (index, kind, payload). Return (blocks, last): the number of valid blocks and the last one as a dict."""
    blocks, last, run = 0, None, []
    for idx, kind, pay in metas:
        if kind == 1 or (run and (kind != len(run) + 1 or idx != run[-1][0] + 1)):
            run = []
        if kind == len(run) + 1:
            run.append((idx, kind, pay))
        if len(run) == 6:
            raw = b"".join(struct.pack("<I", x[2]) for x in run[:5])
            if zlib.crc32(raw) == run[5][2]:
                blocks += 1
                f = struct.unpack("<ffff", raw[4:])
                last = {"rev": run[0][2] & 0xFFFFFF, "flags": run[0][2] >> 24, "pt_a": f[0], "pt_b": f[1], "lc_a": f[2], "lc_b": f[3]}
            run = []
    return blocks, last


def load(path):
    """Return (size, n, flips, pt_only, {"PT": [(t, reading, seq, raw)], "LC": [...], "META": [(index, kind, payload)]})."""
    d = open(path, "rb").read()
    n = len(d) // 8
    recs = list(struct.iter_unpack("<IhH", d[:n * 8]))
    sensor = [r for r in recs if r[0] < META_BASE]
    flips = sum(1 for a, b in zip(sensor, sensor[1:]) if (a[2] ^ b[2]) & 0x8000)
    pt_only = flips * 16384 < len(sensor)
    out = {"PT": [], "LC": [], "META": []}
    for i, (t, v, q) in enumerate(recs):
        raw = d[i * 8:i * 8 + 8]
        if t >= META_BASE:
            out["META"].append((i, t - META_BASE, (v & 0xFFFF) | (q << 16)))
        elif pt_only or not q & 0x8000:
            out["PT"].append((t, v, q & 0x7FFF, raw))
        else:
            c = (v & 0xFFFF) | ((q & 0xFF) << 16)                  # i24, sign extended
            out["LC"].append((t, c - (1 << 24) if c & 0x800000 else c, (q >> 8) & 0x7F, raw))
    return len(d), n, flips, pt_only, out


def stats(name, rec):
    print(f"  {name}: records={len(rec)}", end="")
    if len(rec) < 2:
        print()
        return
    t = [r[0] for r in rec]
    v = [r[1] for r in rec]
    dt = [(b - a) & 0xFFFFFFFF for a, b in zip(t, t[1:])]
    s = sorted(dt)
    med = s[len(s) // 2]
    span = sum(dt) / 1e6
    mod = SEQ_MOD[name]
    lost = sum(((b[2] - a[2]) % mod) - 1 for a, b in zip(rec, rec[1:]))
    # The load cell seq wraps in 0.4 s, so a pause of more than 0.4 s hides in it: the time shows it.
    lost_t = sum(max(round(x / med) - 1, 0) for x in dt if x > 1.5 * med)
    print(f" span={span:.3f} s rate={(len(rec) - 1) / span:.2f}/s lost(seq)={lost} lost(time)={lost_t}")
    print(f"      gap median/p99.9/max={med}/{s[min(int(len(s) * 0.999), len(s) - 1)]}/{s[-1]} us "
          f"max/median={s[-1] / med:.3f} over_1.5xmedian={sum(1 for x in dt if x > 1.5 * med)}")
    print(f"      reading min/mean/max={min(v)}/{sum(v) / len(v):.3f}/{max(v)} counts")


def piece(a, b):
    """Compare the 8-byte records of two lists: is one a contiguous piece of the other, or do they only overlap?"""
    (na, la), (nb, lb) = a, b
    if not la or not lb:
        return "no records in " + (na if not la else nb) + " to compare"
    if len(la) < len(lb):
        (na, la), (nb, lb) = (nb, lb), (na, la)             # la is the longer list
    for (n1, l1), (n2, l2) in (((na, la), (nb, lb)), ((nb, lb), (na, la))):    # does l2 start inside l1?
        i = next((k for k, r in enumerate(l1) if r[3] == l2[0][3]), None)
        if i is None:
            continue
        m = 0
        while m < len(l2) and i + m < len(l1) and l1[i + m][3] == l2[m][3]:
            m += 1
        if m == len(l2):
            kind = "identical to " if len(l1) == len(l2) else "a contiguous piece of "
            return (f"{n2} is {kind}{n1}: {m} of {len(l1)} records, from record {i} "
                    f"({i} before, {len(l1) - i - m} after)")
        if i + m == len(l1):
            return (f"NOT a piece: {n2} starts at record {i} of {n1}, {m} records are equal, then {n1} ends "
                    f"and {n2} goes on for {len(l2) - m} more records")
        return f"NOT a piece: {n2} starts at record {i} of {n1}, and differs at record {m} of {n2} (record {i + m} of {n1})"
    common = len({r[3] for r in lb} & {r[3] for r in la})
    return f"NOT a piece: no first record in common ({common} of the {len(lb)} records of {nb} are in {na})"


def main():
    f = sys.argv[1:]
    if len(f) not in (1, 2):
        sys.exit(__doc__)
    data = []
    for p in f:
        size, n, flips, pt_only, recs = load(p)
        print(f"{p}: {size} bytes, {n} records of 8 bytes" + (f", {size % 8} bytes left over" if size % 8 else "")
              + f", top seq bit flips={flips} -> {'PT-only' if pt_only else 'PT and load cell'}")
        for name in SENSORS:
            stats(name, recs[name])
        blocks, last = decode_cal(recs["META"])
        if recs["META"]:
            print(f"  META records: {len(recs['META'])}, valid calibration blocks: {blocks}"
                  + (f", last: rev {last['rev']}, PT {'set' if last['flags'] & 1 else 'not set'} A={last['pt_a']:.9g} B={last['pt_b']:.9g}"
                     f", LC {'set' if last['flags'] & 2 else 'not set'} A={last['lc_a']:.9g} B={last['lc_b']:.9g}" if last else ""))
        data.append(recs)
    if len(f) == 2:
        print("compare:")
        for name in SENSORS:
            print(f"  {name}: " + piece((f[0], data[0][name]), (f[1], data[1][name])))


if __name__ == "__main__":
    main()
