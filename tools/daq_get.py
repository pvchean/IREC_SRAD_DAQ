#!/usr/bin/env python3
"""PC side of the DAQS, FILES and GET commands (the same protocol as the MATLAB app).

  daq_get.py PORT daqs                 the boards in the chain
  daq_get.py PORT files                the PFS and SD files of the connected board
  daq_get.py PORT get PFS 12 [DIR]     download PFS file 12 to DIR/PFS_F00012D<id>.BIN
  daq_get.py PORT get SD F00012D3.BIN [DIR]
  daq_get.py PORT all [DIR]            download all files

The GET reply is binary: frames [u16 len][len bytes], a frame with len 0, then "GET:END ... crc32=..".
"""
import os
import re
import sys
import time
import zlib

import serial


def line(s, prefixes, timeout=5.0):
    """Read lines until one starts with a prefix. The live stream number lines are skipped."""
    end = time.time() + timeout
    while time.time() < end:
        t = s.readline().decode(errors="replace").strip()
        if t.startswith(prefixes):
            return t
    raise TimeoutError(f"no reply {prefixes}")


def daqs(s):
    s.write(b"DAQS\n")
    m = re.match(r"DAQS:SELF=(\d+) SEEN=([\d,]*)", line(s, ("DAQS:",)))
    return int(m.group(1)), [int(x) for x in m.group(2).split(",") if x]


def files(s):
    s.write(b"FILES\n")
    line(s, ("FILES:BEGIN",))
    out = []
    while True:
        t = line(s, ("FILE ", "FILES:END"))
        if t == "FILES:END":
            return out
        p = t.split()
        out.append((p[1], p[2], int(p[3])))


def get(s, src, name, self_id):
    s.write(f"GET {src} {name}\n".encode())
    t = line(s, ("GET:BEGIN", "GET:ERR"), 10.0)
    if t.startswith("GET:ERR"):
        raise RuntimeError(t)
    data = bytearray()
    t0 = time.time()
    while True:
        h = s.read(2)
        if len(h) < 2:
            raise TimeoutError("frame header")
        n = h[0] | h[1] << 8
        if n == 0:
            break
        b = s.read(n)
        if len(b) < n:
            raise TimeoutError("frame data")
        data += b
    m = re.match(r"GET:END bytes=(\d+) crc32=([0-9A-F]{8}) (\w+)", line(s, ("GET:END",)))
    dt = time.time() - t0
    ok = m and int(m.group(1)) == len(data) and int(m.group(2), 16) == zlib.crc32(data) and m.group(3) == "OK"
    print(f"  {src} {name}: {len(data)} bytes in {dt:.1f} s ({len(data) / max(dt, 1e-3) / 1024:.0f} KiB/s), "
          f"{'CRC OK' if ok else 'BAD ' + str(m.groups() if m else None) + f' local crc {zlib.crc32(data):08X}'}")
    local = f"PFS_F{int(name):05d}D{self_id}.BIN" if src == "PFS" else f"SD_{name}"
    return local, bytes(data), ok


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    s = serial.Serial(a[0], 115200, timeout=3)
    time.sleep(0.2)
    s.reset_input_buffer()
    self_id, seen = daqs(s)
    if a[1] == "daqs":
        print(f"connected to DAQ{self_id}; in the chain: " + " ".join(f"DAQ{i}" for i in sorted(seen + [self_id])))
    elif a[1] == "files":
        for f in files(s):
            print(" ".join(map(str, f)))
    elif a[1] in ("get", "all"):
        todo = [(a[2], a[3])] if a[1] == "get" else [(f[0], f[1]) for f in files(s)]
        out = (a[4] if len(a) > 4 else ".") if a[1] == "get" else (a[2] if len(a) > 2 else ".")
        os.makedirs(out, exist_ok=True)
        for src, name in todo:
            local, data, ok = get(s, src, name, self_id)
            open(os.path.join(out, local), "wb").write(data)
            if not ok:
                sys.exit(1)


if __name__ == "__main__":
    main()
