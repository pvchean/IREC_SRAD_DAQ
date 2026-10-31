#!/usr/bin/env python3
"""Read the serial port from the start, with a time stamp for each line, until a stop text or a time limit.

  read_serial.py PORT SECONDS OUTFILE [STOP_TEXT]
"""
import os
import sys
import time

import serial

port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
stop = sys.argv[4] if len(sys.argv) > 4 else None
t0 = time.time()
while not os.path.exists(port) and time.time() - t0 < 30:
    time.sleep(0.05)
while True:
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
        break
    except (serial.SerialException, OSError):
        time.sleep(0.05)
buf, f = b"", open(out, "w")
while time.time() - t0 < secs:
    buf += s.read(s.in_waiting or 1)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode(errors="replace").rstrip()
        f.write(f"{time.time() - t0:7.2f} {t}\n")
        f.flush()
        if stop and stop in t:
            sys.exit(0)
