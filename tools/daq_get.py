#!/usr/bin/env python3
"""PC side of the DAQS, FILES and CHUNK commands (the same protocol as the MATLAB app).

  daq_get.py PORT daqs                 the boards in the chain
  daq_get.py PORT files                the PFS and SD files of the connected board
  daq_get.py PORT get PFS 12 [DIR]     download PFS file 12 to DIR/NAND_F00012D<id>.BIN
  daq_get.py PORT get SD F00012D3.BIN [DIR]     (to DIR/uSD_F00012D3.BIN)
  daq_get.py PORT all [DIR]            download all files

Download: "CHUNK <src> <name> <offset> <len>" -> "CHUNK:<offset> <n> <crc32>" + n raw bytes; n = 0 at the end.
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
    s.write(b"STREAM OFF\n")
    line(s, ("STREAM:OFF",))
    s.reset_input_buffer()
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
        out.append((p[1], p[2], int(p[3]), int(p[5]) if len(p) > 5 else 0))   # src, name, bytes, start


def get(s, f, self_id):
    src, name, start = f[0], f[1], f[3]
    t0 = time.time()
    data, retries = get_chunks(s, src, name, start)
    dt = time.time() - t0
    print(f"  {src} {name}: {len(data)} bytes in {dt:.1f} s ({len(data) / max(dt, 1e-3) / 1024:.0f} KiB/s), "
          f"{retries} chunks asked for again")
    local = f"NAND_F{int(name):05d}D{self_id}.BIN" if src == "PFS" else f"uSD_{name}"
    return local, data


def get_chunks(s, src, name, start, chunk=16384, tries=5):
    """Download with CHUNK requests. Returns (data, retries). A chunk with an error is asked for again."""
    data, off, retries = bytearray(), start, 0
    while True:
        for t in range(tries):
            s.write(f"CHUNK {src} {name} {off} {chunk}\n".encode())
            hdr = line(s, ("CHUNK:",), 5.0)
            m = re.match(r"CHUNK:(\d+) (\d+) ([0-9A-Fa-f]{8})", hdr)
            err = hdr if hdr.startswith("CHUNK:ERR") else None
            if not err and (not m or int(m.group(1)) != off):
                err = f"bad reply {hdr!r}"
            if not err:
                n = int(m.group(2))
                b = s.read(n)
                if len(b) < n:
                    err = f"only {len(b)} of {n} bytes"
                elif zlib.crc32(b) != int(m.group(3), 16):
                    err = f"CRC DAQ {m.group(3)} PC {zlib.crc32(b):08X}"
            if not err:
                break
            retries += 1
            print(f"    retry at offset {off}: {err}")
            time.sleep(0.3)
            s.reset_input_buffer()
        else:
            raise RuntimeError(f"offset {off}: {err}")
        if n == 0:
            return bytes(data), retries
        data += b
        off += n


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
        lst = files(s)
        todo = [f for f in lst if (f[0], f[1]) == (a[2], a[3])] if a[1] == "get" else lst
        out = (a[4] if len(a) > 4 else ".") if a[1] == "get" else (a[2] if len(a) > 2 else ".")
        os.makedirs(out, exist_ok=True)
        for f in todo:
            local, data = get(s, f, self_id)
            open(os.path.join(out, local), "wb").write(data)


if __name__ == "__main__":
    main()
