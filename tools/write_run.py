#!/usr/bin/env python3
"""ONE recording run for a NAND write test, with the counters before and after. Needs the permission of the user (MEM-028).

  write_run.py PORT SECONDS OUTFILE [CAL_AT_S "CAL PT 13 -3.5"]

Steps: INFO and PROFILE (before), MATLAB PING (live stream on, as the MATLAB app does), REC ON, an optional CAL command at CAL_AT_S,
REC OFF at SECONDS, STREAM OFF, PFSFLUSH, FLUSH, then INFO, PROFILE, LIST, CHECK (after). Every line goes to OUTFILE with its time.
"""
import re
import sys
import threading
import time

import serial

port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
cal_at = float(sys.argv[4]) if len(sys.argv) > 4 else None
cal_cmd = sys.argv[5] if len(sys.argv) > 5 else None
f = open(out, "w")
T0 = time.time()
lock = threading.Lock()
counts = {"num": 0, "lc": 0, "text": 0}
stop = False


def log(t):
    with lock:
        f.write(f"{time.time() - T0:8.2f} {t}\n")
        f.flush()


def reader(s):
    buf = b""
    while not stop:
        try:
            buf += s.read(s.in_waiting or 1)
        except Exception as e:                               # the USB link can drop
            log(f"READER ERROR {e!r}")
            return
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            t = raw.decode(errors="replace").strip()
            if not t:
                continue
            if t.startswith("L:"):
                counts["lc"] += 1
            elif re.match(r"^[+-]?\d+(\.\d+)?$", t):
                counts["num"] += 1
            else:
                counts["text"] += 1
                log(t)


s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3)
s.write(b"\n")
time.sleep(0.3)
s.reset_input_buffer()
th = threading.Thread(target=reader, args=(s,), daemon=True)
th.start()


def cmd(c, wait=0.8):
    log(f">>> {c}")
    s.write((c + "\n").encode())
    time.sleep(wait)


cmd("STREAM OFF")
cmd("INFO", 1.2)
cmd("PROFILE", 1.2)
cmd("CAL GET")
cmd("MATLAB PING", 0.8)
log(f"live counts before REC ON: {counts}")
cmd("REC ON", 0.1)
t_rec = time.time()
did_cal = cal_at is None
while time.time() - t_rec < secs:
    if not did_cal and time.time() - t_rec >= cal_at:
        cmd(cal_cmd, 0.1)
        did_cal = True
    time.sleep(0.05)
cmd("REC OFF", 0.1)
log(f"live counts at REC OFF: {counts}")
cmd("STREAM OFF", 0.8)
cmd("PFSFLUSH", 2.5)
cmd("FLUSH", 2.5)
cmd("INFO", 1.2)
cmd("PROFILE", 1.2)
cmd("LIST", 1.5)
cmd("CHECK", 6.0)
stop = True
log(f"done; REC lasted {time.time() - t_rec:.1f} s; live counts {counts}")
