#!/usr/bin/env python3
"""Reset the board N times. After each boot print the calibration register and the load cell level.

  nau_boots.py PORT N [LOGFILE]
"""
import os
import re
import subprocess
import sys
import time

import serial

ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
PY = os.path.expanduser("~/.platformio/penv/bin/python")


def reset(port):
    subprocess.run([PY, ESPTOOL, "--chip", "esp32s3", "--port", port, "--after", "hard_reset", "read_mac"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)


def one(port):
    reset(port)
    time.sleep(4.0)
    s = serial.Serial(port, 115200, timeout=0.3)
    time.sleep(0.3)
    s.write(b"\n")
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b"INFO\n")
    lc, up = "", ""
    end = time.time() + 2
    while time.time() < end and not lc:
        t = s.readline().decode(errors="replace").strip()
        if t.startswith("LC:"):
            lc = t
        m = re.search(r"up_s=(\d+)", t)
        if m:
            up = m.group(1)
    s.write(b"MATLAB PING\n")
    time.sleep(0.4)
    s.reset_input_buffer()
    b = b""
    end = time.time() + 0.5
    while time.time() < end:
        b += s.read(s.in_waiting or 1)
    s.write(b"STREAM OFF\n")
    time.sleep(0.1)
    s.close()
    v = [int(x) for x in re.findall(rb"^L:(-?\d+)", b, re.M)]
    return lc, up, (sum(v) / len(v) if v else None), len(v)


def main():
    port, n = sys.argv[1], int(sys.argv[2])
    out = open(sys.argv[3], "a") if len(sys.argv) > 3 else None
    bad = 0
    for i in range(n):
        lc, up, mean, cnt = one(port)
        ocal = re.search(r"ocal=(\w+)", lc)
        ok = mean is not None and abs(mean - 280000) < 30000
        bad += 0 if ok else 1
        t = f"boot {i:2d} up_s={up} ocal={ocal.group(1) if ocal else '?'} mean={mean if mean is None else round(mean)} n={cnt} {'ok' if ok else 'BAD'}  {lc[:60]}"
        print(t, flush=True)
        if out:
            out.write(t + "\n")
    print(f"bad {bad} of {n}", flush=True)


if __name__ == "__main__":
    main()
