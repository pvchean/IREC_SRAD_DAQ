#!/usr/bin/env python3
"""Convert DAQ .BIN files (PFS/SD record files, layout in src/record.h) into one combined CSV each.

  bin_to_csv.py FILE.BIN [FILE2.BIN ...] [-o OUTDIR]

Columns: sensor (PT/LC), time_s (from the first sample of the file, both sensors on one clock), time_us (raw timestamp),
seq, raw_count, volts_or_mV_per_V, calibrated_PSI_or_N, cal_A, cal_B, cal_revision.
Calibration = the last valid META block of the file (flags say which sensors are calibrated); empty when none.
Rows are sorted by time. Decoding is the same as rec_stats.py.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rec_stats import decode_cal, load  # noqa: E402

PT_SCALE = 0.1875e-3          # V per ADS1115 count
LC_SCALE = 1000 / 2 ** 31     # mV/V per NAU7802 count (gain 128 * 2^24)


def unwrap(ts):
    out, base, prev = [], 0, None
    for t in ts:
        if prev is not None and t < prev:
            base += 1 << 32
        out.append(t + base)
        prev = t
    return out


def convert(path, outdir):
    _, _, _, _, recs = load(path)
    _, cal = decode_cal(recs["META"])
    rows = []
    for name, scale, a_key, b_key, bit in (("PT", PT_SCALE, "pt_a", "pt_b", 1), ("LC", LC_SCALE, "lc_a", "lc_b", 2)):
        r = recs[name]
        full = unwrap([x[0] for x in r])
        use = cal is not None and cal["flags"] & bit
        for (t, v, q, _), tu in zip(r, full):
            nat = v * scale
            rows.append((tu, name, t, q, v, nat,
                         cal[a_key] * nat + cal[b_key] if use else "",
                         cal[a_key] if use else "", cal[b_key] if use else "", cal["rev"] if use else ""))
    rows.sort(key=lambda x: x[0])
    t0 = rows[0][0] if rows else 0
    out = os.path.join(outdir, os.path.splitext(os.path.basename(path))[0] + ".csv")
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["sensor", "time_s", "time_us", "seq", "raw_count", "volts_or_mV_per_V", "calibrated_PSI_or_N",
                    "cal_A", "cal_B", "cal_revision"])
        for tu, name, t, q, v, nat, cv, a, b, rev in rows:
            w.writerow([name, f"{(tu - t0) / 1e6:.6f}", t, q, v, f"{nat:.9g}",
                        f"{cv:.9g}" if cv != "" else "", f"{a:.9g}" if a != "" else "", f"{b:.9g}" if b != "" else "", rev])
    print(f"{path} -> {out}: {len(rows)} samples, calibration {'rev %d' % cal['rev'] if cal else 'none'}")


def main():
    args, outdir = sys.argv[1:], None
    if "-o" in args:
        i = args.index("-o")
        outdir = args[i + 1]
        args = args[:i] + args[i + 2:]
        os.makedirs(outdir, exist_ok=True)
    if not args:
        sys.exit(__doc__)
    for p in args:
        convert(p, outdir or os.path.dirname(os.path.abspath(p)))


if __name__ == "__main__":
    main()
