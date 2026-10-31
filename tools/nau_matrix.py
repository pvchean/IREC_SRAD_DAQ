#!/usr/bin/env python3
"""Noise matrix of the NAU7802 with the daq_nau_probe firmware (Phase 0 test 5).

  nau_matrix.py PORT OUTDIR [SECONDS]

Each row prints the rate, the total noise, and the noise with the mains bins removed.
"""
import os
import sys
import time
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nau_bench as nb  # noqa: E402

VLDO = nb.VLDO
RATE = {3: 80, 7: 320}


def collect(s, secs):
    s.write(b"NAU GET\n")
    end = time.time() + 5
    while time.time() < end:
        t = s.readline().decode(errors="replace").strip()
        if t.startswith("NAU:DATA "):
            break
    else:
        sys.exit("no NAU:DATA")
    n, crc = int(t.split()[1]), int(t.split()[2], 16)
    s.timeout = 15
    b = s.read(n * 8)
    s.timeout = 0.3
    if len(b) != n * 8 or zlib.crc32(b) != crc:
        sys.exit("bad data")
    return b


def analyse(b, ldo_v):
    d = np.frombuffer(b, dtype=[("t", "<u4"), ("v", "<i4")])
    v = d["v"].astype(float)[8:]                       # the first samples settle
    t = d["t"].astype(float)[8:]
    span = (np.diff(d["t"].astype(np.int64)) % (1 << 32)).sum() / 1e6
    fs = (len(d) - 1) / span
    lsb = ldo_v / (128 * 2 ** 24) * 1e9
    k = np.arange(len(v))
    x = v - np.polyval(np.polyfit(k, v, 1), k)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / fs)
    p = np.abs(X) ** 2 * 2 / len(x) ** 2
    mains = np.zeros(len(p), dtype=bool)
    for h in range(60, int(fs / 2), 60):
        mains |= np.abs(f - h) < 0.75
    # A tone above fs/2 folds back into the band.
    a60 = np.sqrt(p[np.abs(f - 60) < 0.75].sum() * 2) if fs > 120 else 0.0
    rms = np.sqrt(p.sum())
    rms_nm = np.sqrt(p[~mains].sum())
    return fs, rms * lsb, rms_nm * lsb, a60 * lsb, v.mean() * lsb / 1000


def main():
    port, out = sys.argv[1], sys.argv[2]
    secs = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    s = nb.open_port(port)
    runs = []
    for ldo in (0, 1, 2):
        for crs in (7, 3):
            for cap in (1, 0):
                runs.append((ldo, crs, cap, 0))
    runs.insert(0, (0, 7, 1, 1))                       # inputs shorted inside the chip (SI)
    runs.insert(1, (0, 3, 1, 1))
    print(" LDO  SPS cap short |    rate   total nV  no-mains nV  60 Hz amp nV   mean uV", flush=True)
    for ldo, crs, cap, short in runs:
        nb.ask(s, f"NAU INIT {ldo} 7 {crs} {cap}", "NAU:INIT", 12)
        if short:
            nb.ask(s, "NAU WR 0x11 0x08", "NAU:WR", 2)
            time.sleep(0.2)
        r = nb.ask(s, f"NAU RUN {secs}", "NAU:RUN", secs + 5)
        run = [x for x in r if x.startswith("NAU:RUN")]
        b = collect(s, secs)
        open(os.path.join(out, f"m_l{ldo}_c{crs}_p{cap}_s{short}.bin"), "wb").write(b)
        fs, tot, nm, a60, mean = analyse(b, VLDO[ldo])
        print(f"{VLDO[ldo]:4.1f} {RATE[crs]:4d}  {cap}    {short}   | {fs:7.2f} {tot:9.1f} {nm:11.1f} {a60:12.1f} {mean:9.2f}   "
              f"{run[0][8:] if run else 'NO RUN LINE'}", flush=True)
    nb.ask(s, "NAU WR 0x11 0x00", "NAU:WR", 2)


if __name__ == "__main__":
    main()
