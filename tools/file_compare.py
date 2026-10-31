#!/usr/bin/env python3
"""Download PFS files with CHUNK and compare each with a copy that was saved earlier (at the old NAND clock).

  file_compare.py PORT ID=PATH [ID=PATH ...]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import daq_get as dg  # noqa: E402
import serial  # noqa: E402

port = sys.argv[1]
s = serial.Serial(port, 115200, timeout=3)
time.sleep(0.3)
s.write(b"\n")
time.sleep(0.2)
s.reset_input_buffer()
sid, _ = dg.daqs(s)
files = {f[1]: f for f in dg.files(s) if f[0] == "PFS"}
ok_all = True
for a in sys.argv[2:]:
    fid, path = a.split("=", 1)
    f = files.get(fid)
    if not f:
        print(f"file {fid}: not on the board"); ok_all = False; continue
    t0 = time.time()
    data, retries = dg.get_chunks(s, "PFS", fid, f[3])
    ref = open(path, "rb").read()
    same = data == ref
    first = next((i for i in range(min(len(data), len(ref))) if data[i] != ref[i]), None)
    print(f"file {fid}: {len(data)} bytes in {time.time() - t0:.1f} s, retries {retries}, reference {len(ref)} bytes -> "
          + ("IDENTICAL" if same else f"DIFFERENT (first difference at byte {first}, sizes {len(data)} / {len(ref)})"), flush=True)
    ok_all &= same
print("ALL IDENTICAL" if ok_all else "MISMATCH")
