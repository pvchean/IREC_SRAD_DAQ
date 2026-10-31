#!/usr/bin/env python3
"""Run the serial sequence of DAQ_Calibration_App.m against a board, step by step, and check each reply.

  matlab_sim.py PORT [DIR]

Steps (the same order and bytes as the MATLAB app):
  1. "MATLAB PING" without a line end, wait 1.5 s, find the banner (Auto-Connect)
  2. live numbers come, and "L:<count>" lines of the load cell (text for the app: the numbers do not count them);
     "DAQS\\n" during the stream; parse the reply with the app's regex
  3. "STREAM OFF" -> "STREAM:OFF", then no more number lines and no more L: lines
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
LC_RE = re.compile(r"L:-?\d+")                            # a load cell line


def read_for(s, seconds):
    """Read everything that comes in `seconds`. One read of in_waiting gets 4095 bytes at most (the tty buffer): that is
    only half a second of the stream with the L: lines."""
    out, end = b"", time.time() + seconds
    while time.time() < end:
        out += s.read(max(1, s.in_waiting))
    return out.decode(errors="replace")


def split_live(chunk):
    """Number lines and load cell lines of a piece of the live stream. L: lines are text lines: no number line."""
    lines = chunk.splitlines()
    if lines and not chunk.endswith("\n"):
        lines = lines[:-1]                                    # the last line can be cut
    nums = [l for l in lines if l and not TXT_RE.match(l)]
    return nums, [l for l in lines if l.startswith("L:")]


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
            try:
                hdr = line(s, ("CHUNK:",), 5.0)
            except TimeoutError:      # a header can be lost: ask again, as the MATLAB app does
                retries += 1
                print(f"    retry at offset {off}: no reply")
                time.sleep(0.3)
                s.reset_input_buffer()
                continue
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
    s.write(b"\n")                    # the first command after the open is garbled: this ends the broken line
    time.sleep(0.2)
    s.reset_input_buffer()

    s.write(b"MATLAB PING")                                   # no line end, as write(s, "MATLAB PING", "string")
    resp = read_for(s, 1.5)
    step(BANNER.decode() in resp, "1 banner after MATLAB PING without a line end")

    s.reset_input_buffer()
    chunk = read_for(s, 1.0)
    nums, lcs = split_live(chunk.split("\n", 1)[-1])         # the first line can be cut
    step(len(nums) > 500, f"2a live stream: {len(nums)} number lines in about 1 s (expect about 860)")
    bad = [l for l in lcs if not LC_RE.fullmatch(l)]
    step(len(lcs) > 150 and not bad, f"2c live stream: {len(lcs)} load cell L: lines in about 1 s (expect about 320), "
                                     f"{len(bad)} not of the form L:<integer>")

    s.write(b"DAQS\n")
    chunk = read_for(s, 0.3)
    txt = [t.strip() for t in TXT_RE.findall(chunk) if not t.startswith("L:")]     # the L: lines are not the reply
    m = [re.search(r"DAQS:SELF=(\d+) SEEN=([\d,]*)", t) for t in txt]
    m = [x for x in m if x]
    step(bool(m), f"2b DAQS reply found between the numbers: {txt}")
    print(f"     connected DAQ{m[0].group(1)}, others: {m[0].group(2) or '(none)'}")

    s.write(b"STREAM OFF\n")
    step(wait_line(s, ("STREAM:OFF",)) is not None, "3a STREAM:OFF reply")
    time.sleep(0.5)
    rest = s.read(s.in_waiting).decode(errors="replace")
    late, late_lc = split_live(rest)
    step(not late and not late_lc, f"3b no number lines and no L: lines after STREAM:OFF ({len(late)} and {len(late_lc)} found)")

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
    chunk = read_for(s, 1.0)
    nums, lcs = split_live(chunk.split("\n", 1)[-1])
    step(len(nums) > 500, f"5 stream on again: {len(nums)} number lines and {len(lcs)} L: lines in about 1 s")
    s.write(b"STREAM OFF\n")
    print("ALL PASS")


if __name__ == "__main__":
    main()
