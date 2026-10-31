#!/usr/bin/env python3
"""Ask for CHUNK many times and report each reply that does not come. Shows what arrived instead.

  chunk_stress.py PORT PFS_FILE_ID COUNT
"""
import sys
import time
import zlib

import serial

port, fid, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3)
s.write(b"\n")
time.sleep(0.2)
s.write(b"STREAM OFF\n")
time.sleep(0.3)
s.reset_input_buffer()
CH = 16384
off, size, fails, bad, t0, wrap = 0, 5055008, 0, 0, time.time(), 0
for i in range(count):
    t1 = time.time()
    s.write(f"CHUNK PFS {fid} {off} {CH}\n".encode())
    raw, hdr = b"", None
    while time.time() - t1 < 6:
        raw += s.read(s.in_waiting or 1)
        k = raw.find(b"CHUNK:")
        if k >= 0 and b"\n" in raw[k:]:
            hdr = raw[k:raw.index(b"\n", k)].decode(errors="replace").strip()
            raw = raw[raw.index(b"\n", k) + 1:]
            break
    if not hdr:
        fails += 1
        print(f"#{i} off={off} NO HEADER after {time.time() - t1:.1f}s, got {len(raw)} bytes: {raw[:120]!r}", flush=True)
        s.write(b"\nDAQS\n")
        time.sleep(0.4)
        print("  then:", s.read(s.in_waiting or 1)[:120], flush=True)
        s.reset_input_buffer()
        continue
    if hdr.startswith("CHUNK:ERR"):
        print(f"#{i} off={off} {hdr}", flush=True)
        continue
    _, n, crc = hdr.split()
    n = int(n)
    if n == 0:
        off = 0
        wrap += 1
        continue
    s.timeout = 3
    while len(raw) < n:
        b = s.read(n - len(raw))
        if not b:
            break
        raw += b
    s.timeout = 0.2
    if len(raw) < n or zlib.crc32(raw[:n]) != int(crc, 16):
        bad += 1
        print(f"#{i} off={off} short or bad CRC: {len(raw)} of {n}", flush=True)
        time.sleep(0.3)
        s.reset_input_buffer()
        continue
    off += n
print(f"done {count} requests, {time.time() - t0:.0f} s, no header {fails}, bad data {bad}, file passes {wrap}", flush=True)
