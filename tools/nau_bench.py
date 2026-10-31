#!/usr/bin/env python3
"""Bring-up bench of the NAU7802 with the daq_nau_probe firmware.

  nau_bench.py PORT cmd "NAU SCAN" ["NAU DUMP" ...]     send commands, print the replies
  nau_bench.py PORT run LDO GAIN CRS CAP SECONDS [OUT]  INIT, RUN, GET, print the statistics
  nau_bench.py stats FILE                               statistics of a saved run
"""
import math
import re
import statistics as st
import struct
import sys
import time
import zlib

import serial

VLDO = [4.5, 4.2, 3.9, 3.6, 3.3, 3.0, 2.7, 2.4]
GAIN = [1, 2, 4, 8, 16, 32, 64, 128]


def open_port(port):
    s = serial.Serial(port, 115200, timeout=0.3)
    time.sleep(0.3)
    s.write(b"\n")            # the tty can echo text before raw mode: this ends a broken first line
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def ask(s, cmd, want=None, timeout=3.0):
    """Send a line. Return the text lines until a line with `want`, or until the timeout."""
    s.write((cmd + "\n").encode())
    out, end = [], time.time() + timeout
    while time.time() < end:
        t = s.readline().decode(errors="replace").rstrip()
        if not t:
            continue
        out.append(t)
        if want and want in t:
            break
    return out


def stats(recs, ldo, gain, label=""):
    t = [r[0] for r in recs]
    v = [r[1] for r in recs]
    dt = [(b - a) & 0xFFFFFFFF for a, b in zip(t, t[1:])]
    sdt = sorted(dt)
    span = sum(dt) / 1e6
    lsb = ldo / (gain * 2 ** 24)
    # A line fit removes the drift, so the number is the noise of the run.
    n = len(v)
    tt = [i - (n - 1) / 2 for i in range(n)]
    my = sum(v) / n
    k = sum(a * (b - my) for a, b in zip(tt, v)) / sum(a * a for a in tt)
    res = [b - my - k * a for a, b in zip(tt, v)]
    rms = math.sqrt(sum(x * x for x in res) / n)
    pp = max(res) - min(res)
    fs = 2 ** 24
    print(f"{label}n={n} span={span:.1f}s rate={(n - 1) / span:.2f}/s dt med/p99.9/max={sdt[len(sdt) // 2]}/"
          f"{sdt[int(len(sdt) * .999)]}/{sdt[-1]} us over_1.5T={sum(1 for x in dt if x > 1.5 * sdt[len(sdt) // 2])}")
    print(f"  mean={my:.1f} counts ({my * lsb * 1e6:.2f} uV) drift={k * (n - 1):.1f} counts  rms={rms:.1f} counts "
          f"({rms * lsb * 1e9:.1f} nV)  p-p={pp:.0f}  ENOB={math.log2(fs / rms):.2f}  noise-free={math.log2(fs / (6.6 * rms)):.2f} bits")
    return rms * lsb


def run(s, ldo, gain, crs, cap, secs, out=None):
    print(f"--- LDO {VLDO[ldo]} V gain {GAIN[gain]} crs {crs} cap {cap}, {secs} s")
    for t in ask(s, f"NAU INIT {ldo} {gain} {crs} {cap}", "NAU:INIT", 12):
        print(" ", t)
    for t in ask(s, f"NAU RUN {secs}", "NAU:RUN", secs + 5):
        print(" ", t)
    s.write(b"NAU GET\n")
    end = time.time() + 5
    while time.time() < end:
        t = s.readline().decode(errors="replace").strip()
        m = re.match(r"NAU:DATA (\d+) ([0-9A-Fa-f]{8})", t)
        if m:
            break
    else:
        sys.exit("no NAU:DATA reply")
    n = int(m.group(1))
    s.timeout = 10
    b = s.read(n * 8)
    s.timeout = 0.3
    if len(b) != n * 8 or zlib.crc32(b) != int(m.group(2), 16):
        sys.exit(f"bad data: {len(b)} of {n * 8} bytes")
    if out:
        open(out, "wb").write(b)
    return stats(list(struct.iter_unpack("<Ii", b)), VLDO[ldo], GAIN[gain])


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "stats":
        d = open(a[1], "rb").read()
        stats(list(struct.iter_unpack("<Ii", d)), 4.5, 128)
        return
    if len(a) < 2:
        sys.exit(__doc__)
    s = open_port(a[0])
    if a[1] == "cmd":
        for c in a[2:]:
            print(">>>", c)
            for t in ask(s, c, None, 1.5):
                print(t)
    elif a[1] == "run":
        run(s, *map(int, a[2:7]), a[7] if len(a) > 7 else None)


if __name__ == "__main__":
    main()
