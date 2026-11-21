#!/usr/bin/env python3
"""Convert DAQ .BIN files (PFS/SD record files, layout in src/record.h) into one combined CSV each.

  bin_to_csv.py FILE.BIN [FILE2.BIN ...] [-o OUTDIR]

Columns: sensor (PT/LC), time_s (from the first sample of the file, both sensors on one clock), time_us (raw timestamp),
seq, raw_count, volts_or_mV_per_V, calibrated_PSI_or_N, cal_A, cal_B, cal_revision.
Calibration = the last valid META block of the file (flags say which sensors are calibrated); empty when none.
Rows are sorted by time. Decoding is the same as rec_stats.py.
"""
import csv
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rec_stats import decode_cal, decode_ntc, load  # noqa: E402

PT_SCALE = 0.1875e-3          # V per ADS1115 count
LC_SCALE = 1000 / 2 ** 31     # mV/V per NAU7802 count (gain 128 * 2^24)


# NTC thermistors (record.h kinds 12 and 13): 5 V - NTC - node - fixed resistor - GND. The part is not known to the firmware,
# so the temperature is computed here with the beta equation. Type 0 = 10 kohm, 1 = 100 kohm (bit 15 of each count).
# Change with --ntc-beta10 --ntc-beta100 --ntc-vs --ntc-rfixed.
NTC = {"r25": (10000.0, 100000.0), "beta": [3950.0, 3950.0], "vs": 5.0, "rfixed": 560.0}


NTC_SCALE = 0.125e-3          # V per count: the NTC channels read at +/-4.096 V


def ntc_temp_c(raw, typ):
    v = raw * NTC_SCALE
    if v <= 0 or v >= NTC["vs"]:
        return None
    r = NTC["rfixed"] * (NTC["vs"] / v - 1)
    return 1 / (1 / 298.15 + math.log(r / NTC["r25"][typ]) / NTC["beta"][typ]) - 273.15


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
    ntc = decode_ntc(recs["META"])
    ntc_t = unwrap([x[0] for x in ntc])
    starts = ([rows[0][0]] if rows else []) + ntc_t[:1]
    t0 = min(starts) if starts else 0
    if ntc:
        nout = os.path.join(outdir, os.path.splitext(os.path.basename(path))[0] + "_NTC.csv")
        with open(nout, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time_s", "time_us", "ain2_type", "ain2_raw", "ain2_volts", "ain2_temp_C",
                        "ain3_type", "ain3_raw", "ain3_volts", "ain3_temp_C"])
            for (t, a, b), tu in zip(ntc, ntc_t):
                cells = []
                for ch in (a, b):
                    if ch is None:
                        cells += ["", "", "", ""]
                        continue
                    raw, typ = ch
                    tc = ntc_temp_c(raw, typ)
                    cells += ["100k" if typ else "10k", raw, f"{raw * NTC_SCALE:.6f}", f"{tc:.3f}" if tc is not None else ""]
                w.writerow([f"{(tu - t0) / 1e6:.6f}", t] + cells)
        print(f"{path} -> {nout}: {len(ntc)} temperature readings (beta {NTC['beta'][0]:.0f} / {NTC['beta'][1]:.0f}, "
              f"{NTC['vs']} V, {NTC['rfixed']:.0f} ohm)")
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
    for flag, key in (("--ntc-beta10", 0), ("--ntc-beta100", 1), ("--ntc-vs", "vs"), ("--ntc-rfixed", "rfixed")):
        if flag in args:
            i = args.index(flag)
            if isinstance(key, int):
                NTC["beta"][key] = float(args[i + 1])
            else:
                NTC[key] = float(args[i + 1])
            args = args[:i] + args[i + 2:]
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
