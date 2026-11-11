#!/usr/bin/env python3
# usage: host_cdc.py PORT CMD [wait_s]   sends one CDC command, prints the reply (helper for Claude)
import sys, serial, time
p = serial.Serial(sys.argv[1], 115200, timeout=0.5)
p.write((sys.argv[2] + "\r\n").encode())
t = time.time() + float(sys.argv[3] if len(sys.argv) > 3 else 2)
while time.time() < t:
    d = p.read(256)
    if d: sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
