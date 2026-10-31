#!/usr/bin/env python3
"""Restore the top-level files of managed_components/joltwallet__littlefs that the component manager can drop when a new
PlatformIO env builds for the first time ("The downloaded component is corrupted ... README.md is missing").
Copies the missing files from the verified cache copy (the version of dependencies.lock). Checks every sha256."""
import hashlib
import json
import os
import shutil

c = os.path.expanduser("~/.cache/Espressif/ComponentManager/service_d92d8f1e/joltwallet__littlefs_1.22.3_3af20a3d")
m = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "managed_components", "joltwallet__littlefs")
files = json.load(open(os.path.join(c, "CHECKSUMS.json")))["files"]
n = 0
for f in files:
    dst = os.path.join(m, f["path"])
    if not os.path.exists(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(c, f["path"]), dst)
        n += 1
bad = [f["path"] for f in files if hashlib.sha256(open(os.path.join(m, f["path"]), "rb").read()).hexdigest() != f["hash"]]
print(f"copied {n}, bad {len(bad)} {bad[:3]}")
