#!/usr/bin/env python3
"""Run the serial sequence of DAQ_Calibration_App.m against a board, step by step, and check each reply.

  matlab_sim.py PORT [DIR]

Steps (the same order and bytes as the MATLAB app):
  1. "MATLAB PING" without a line end, wait 1.5 s, find the banner (Auto-Connect)
  2. live numbers come; "DAQS\\n" during the stream; parse the reply with the app's regex
  3. "STREAM OFF" -> "STREAM:OFF", then no more number lines
  4. "FILES" -> list; CHUNK download of the newest PFS file and of each SD file, CRC-32 per chunk
  5. "STREAM ON" -> numbers come again
"""
import re
import sys
import time
import zlib

import serial

BANNER = b"01010101 10101010 DAQBOARD V1 01010101 10101010"
TXT_RE = re.compile(r"^[^0-9+\-.\r\n][^\r\n]*", re.M)     # the app's text line regex


def step(ok, text):
    print(f"[{'PASS' if ok else 'FAIL'}] {text}")
    if not ok:
        sys.exit(1)


def line(s, prefixes, timeout=5.0):
    t = wait_line(s, prefixes, timeout)
    if t is None:
        raise TimeoutError(f"no reply {prefixes}")
    return t


def wait_line(s, prefixes, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        t = s.readline().decode(errors="replace").strip()
        if t.startswith(prefixes):
            return t
    return None


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
    port = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    s = serial.Serial(port, 115200, timeout=3)
    time.sleep(0.3)
    s.reset_input_buffer()

    s.write(b"MATLAB PING")                                   # no line end, as write(s, "MATLAB PING", "string")
    time.sleep(1.5)
    resp = s.read(s.in_waiting)
    step(BANNER in resp, "1 banner after MATLAB PING without a line end")

    s.reset_input_buffer()
    time.sleep(1.0)
    chunk = s.read(s.in_waiting).decode(errors="replace")
    nums = [l for l in chunk.splitlines() if l and not TXT_RE.match(l)]
    step(len(nums) > 500, f"2a live stream: {len(nums)} number lines in about 1 s (expect about 860)")

    s.write(b"DAQS\n")
    time.sleep(0.3)
    chunk = s.read(s.in_waiting).decode(errors="replace")
    txt = [t.strip() for t in TXT_RE.findall(chunk)]
    m = [re.search(r"DAQS:SELF=(\d+) SEEN=([\d,]*)", t) for t in txt]
    m = [x for x in m if x]
    step(bool(m), f"2b DAQS reply found between the numbers: {txt}")
    print(f"     connected DAQ{m[0].group(1)}, others: {m[0].group(2) or '(none)'}")

    s.write(b"STREAM OFF\n")
    step(wait_line(s, ("STREAM:OFF",)) is not None, "3a STREAM:OFF reply")
    time.sleep(0.5)
    rest = s.read(s.in_waiting).decode(errors="replace")
    late = [l for l in rest.splitlines() if l and not TXT_RE.match(l)]
    step(not late, f"3b no number lines after STREAM:OFF ({len(late)} found)")

    s.reset_input_buffer()
    s.write(b"FILES\n")
    step(wait_line(s, ("FILES:BEGIN",)) is not None, "4a FILES:BEGIN")
    files = []
    while True:
        t = wait_line(s, ("FILE ", "FILES:END"))
        if t is None or t == "FILES:END":
            break
        files.append(t.split())
    step(t == "FILES:END", f"4b FILES:END, {len(files)} files: " + ", ".join(f"{f[1]} {f[2]} ({f[3]} B)" for f in files))

    pfs = [f for f in files if f[1] == "PFS"]
    todo = ([max(pfs, key=lambda f: int(f[2]))] if pfs else []) + [f for f in files if f[1] == "SD"]
    for f in todo:
        start = int(f[5]) if f[1] == "PFS" and len(f) > 5 else 0
        t0 = time.time()
        data, retries = get_chunks(s, f[1], f[2], start)
        dt = time.time() - t0
        step(retries == 0, f"4c {f[1]} {f[2]}: {len(data)} B (FILES said {f[3]}), "
                           f"{len(data) / max(dt, 1e-3) / 1024:.0f} KiB/s, {retries} retries, whole records: {len(data) % 8 == 0}")
        if out:
            open(f"{out}/{f[1]}_{f[2]}.BIN", "wb").write(data)

    s.reset_input_buffer()
    s.write(b"STREAM ON\n")
    time.sleep(1.0)
    chunk = s.read(s.in_waiting).decode(errors="replace")
    nums = [l for l in chunk.splitlines() if l and not TXT_RE.match(l)]
    step(len(nums) > 500, f"5 stream on again: {len(nums)} number lines in about 1 s")
    s.write(b"STREAM OFF\n")
    print("ALL PASS")


if __name__ == "__main__":
    main()
