#!/usr/bin/env python3
"""Reboot the board N times with the REBOOT command of a daq_profile build and collect the PROFILE lines.

  boot_bench.py PORT N plain|wrap OUTFILE      (OUTFILE gets one line of text for each boot; a summary is printed)
"""
import os
import re
import statistics as st
import sys
import time

import serial


def open_port(port, wait=30):
    t0 = time.time()
    while time.time() - t0 < wait:
        try:
            if os.path.exists(port):
                s = serial.Serial(port, 115200, timeout=0.3)
                return s
        except (serial.SerialException, OSError):
            pass
        time.sleep(0.1)
    raise RuntimeError("port did not come back")


def ask(s, cmd, want, timeout=4.0):
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    out, end = [], time.time() + timeout
    while time.time() < end:
        t = s.readline().decode(errors="replace").strip()
        if t:
            out.append(t)
            if want and t.startswith(want):
                break
    return out


def kv(line):
    return {m.group(1): float(m.group(2)) for m in re.finditer(r"([a-z_]+)=(-?\d+(?:\.\d+)?)", line)}


def one(port, wrap):
    s = open_port(port)
    s.write(b"\n")
    time.sleep(0.2)
    try:
        s.write(b"REBOOT WRAP\n" if wrap else b"REBOOT\n")
        time.sleep(0.3)
        s.close()
    except (serial.SerialException, OSError):
        pass
    time.sleep(1.5)
    s = open_port(port)
    time.sleep(0.5)
    s.write(b"\n")
    time.sleep(0.2)
    res = {}
    end = time.time() + 40
    while time.time() < end:
        lines = ask(s, "PROFILE", "STATUS2:", 3)
        b = next((x for x in lines if x.startswith("BOOT:")), None)
        if b and kv(b.split("|", 1)[1].split("ready:")[1].split("|")[0] if "ready:" in b else "").get("file_index", -1) > 0:
            res = {"BOOT": b, "lines": lines}
            break
        time.sleep(1.0)
    s.close()
    return res


def main():
    port, n, mode, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    rows = []
    f = open(out, "a")
    for i in range(n):
        r = one(port, mode == "wrap")
        if not r:
            print(f"boot {i}: no PROFILE reply"); continue
        for l in r["lines"]:
            f.write(f"boot{i} {l}\n")
        f.flush()
        b = r["BOOT"]
        pre = kv(b.split("|")[0])
        ms = {k: v for k, v in kv(b.split("|", 1)[1]).items()}
        rows.append((pre.get("pre_app_ms", -1), ms))
        print(f"boot {i}: pre_app={pre.get('pre_app_ms')} mount={ms.get('mount_ms')} sd={ms.get('sd_mounted')} index={ms.get('file_index')} load_cell={ms.get('load_cell')}", flush=True)
    for k in ("mount_ms", "app_main", "mount_start", "mount_end", "cmd", "ads", "load_cell", "sd_mounted", "file_index"):
        v = [r[1][k] for r in rows if k in r[1]]
        if v:
            print(f"  {k:12s} median {st.median(v):8.1f}  min {min(v):8.1f}  max {max(v):8.1f}  (n={len(v)})")
    v = [r[0] for r in rows if r[0] > 0]
    if v:
        print(f"  pre_app_ms   median {st.median(v):8.1f}  min {min(v):8.1f}  max {max(v):8.1f}  (n={len(v)})")


if __name__ == "__main__":
    main()
