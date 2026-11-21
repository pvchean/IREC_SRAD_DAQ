#!/usr/bin/env python3
"""Python copy of patchCalBlocks / calFromMeta in src/DAQ_Calibration_App.m (no MATLAB here), with the 1-based record
numbers of the MATLAB code, and a test on copies of real .BIN files.

  cal_patch_mirror.py test FILE_WITH_BLOCKS FILE_WITHOUT_BLOCKS     (works on copies in a temp folder; the files are not changed)
  cal_patch_mirror.py show FILE...                                   print the last valid calibration block of each file
"""
import os, shutil, struct, sys, tempfile, zlib
import numpy as np

META = 0xFFFFFFF0

def cal_from_meta(kind, pay, idx):
    """kind: META kind of each META record, pay: their 4 payload bytes (M x 4), idx: their 1-based record numbers."""
    cal = dict(valid=False, rev=0, ptSet=False, lcSet=False, ptA=np.nan, ptB=np.nan, lcA=np.nan, lcB=np.nan, nBlocks=0)
    blk = []
    M = len(kind)
    for i in [j for j in range(max(M - 5, 0)) if kind[j] == 1]:
        if list(kind[i:i + 6]) != [1, 2, 3, 4, 5, 6] or idx[i + 5] - idx[i] != 5:
            continue
        body = pay[i:i + 5].tobytes()
        if zlib.crc32(body) != struct.unpack("<I", pay[i + 5].tobytes())[0]:
            continue
        hd = struct.unpack("<I", body[:4])[0]
        f = struct.unpack("<4f", body[4:])
        cal.update(valid=True, nBlocks=cal["nBlocks"] + 1, rev=hd & 0xFFFFFF, ptSet=bool(hd >> 24 & 1), lcSet=bool(hd >> 24 & 2),
                   ptA=f[0], ptB=f[1], lcA=f[2], lcB=f[3])
        blk.append(idx[i])
    return cal, blk

def whole(path):
    b = np.fromfile(path, np.uint8)
    return b[:b.size // 8 * 8].reshape(-1, 8)

def read_cal(path):
    b = np.fromfile(path, np.uint8)
    rec = b[:b.size // 8 * 8].reshape(-1, 8)             # a part of a record at the end is ignored
    ts = rec[:, :4].copy().view("<u4").ravel().astype(np.int64)
    meta = ts >= META
    idx = np.nonzero(meta)[0] + 1                       # 1-based, like find(isMeta)
    return cal_from_meta(ts[meta] - META, rec[meta][:, 4:8], idx)

def patch_cal_blocks(path, is_set, vals):
    b = np.fromfile(path, np.uint8)
    if b.size < 8:
        raise ValueError("The file has no records")
    c0, blk = read_cal(path)
    rev = (c0["rev"] if c0["valid"] else 0) + 1
    rev %= 2 ** 24
    hd = ((int(is_set[0]) + 2 * int(is_set[1])) << 24) | rev
    pay = [struct.pack("<I", hd)] + [struct.pack("<f", v) for v in vals]
    pay.append(struct.pack("<I", zlib.crc32(b"".join(pay[:5]))))
    with open(path, "r+b") as f:
        if not blk:
            nr = b.size // 8
            f.seek(nr * 8)                                # after the last whole record
            for j in range(1, 7):
                f.write(struct.pack("<I", META + j) + pay[j - 1])
            f.write(b[nr * 8:].tobytes())                 # the part of a record at the end moves behind the new block
            return 1
        for i in blk:
            for j in range(1, 7):
                f.seek((i + j - 2) * 8 + 4)             # the MATLAB offset: payload of record number i + j - 1 (1-based)
                f.write(pay[j - 1])
        return len(blk)

def test(with_blocks, without_blocks):
    tmp = tempfile.mkdtemp()
    vals = [494.586, 0.0, 1196.14 * 4.4482216152605, 0.0]
    f32 = [float(np.float32(v)) for v in vals]
    # 1. a file with blocks: every block is rewritten in place, nothing else changes
    p = os.path.join(tmp, "a.bin"); shutil.copy(with_blocks, p)
    before = np.fromfile(p, np.uint8); tail = before[before.size // 8 * 8:].copy(); before = before[:before.size // 8 * 8].reshape(-1, 8); c_old, blk_old = read_cal(p)
    n = patch_cal_blocks(p, (True, True), vals)
    raw_a = np.fromfile(p, np.uint8); assert (raw_a[raw_a.size // 8 * 8:] == tail).all(), 'the partial record at the end changed'; after = raw_a[:raw_a.size // 8 * 8].reshape(-1, 8); c_new, blk_new = read_cal(p)
    meta_rows = np.zeros(len(before), bool)
    for i in blk_old: meta_rows[i - 1:i + 5] = True
    assert before.shape == after.shape and blk_old == blk_new and n == len(blk_old) == c_old["nBlocks"] == c_new["nBlocks"]
    assert (before[~meta_rows] == after[~meta_rows]).all(), "a sample record changed"
    assert (before[meta_rows][:, :4] == after[meta_rows][:, :4]).all(), "a META time field changed"
    assert c_new["rev"] == c_old["rev"] + 1 and c_new["ptSet"] and c_new["lcSet"]
    assert [c_new[k] for k in ("ptA", "ptB", "lcA", "lcB")] == f32, [c_new[k] for k in ("ptA", "ptB", "lcA", "lcB")]
    print("blocks: %d rewritten, rev %d -> %d, samples untouched (%d records), values %s" % (n, c_old["rev"], c_new["rev"],
          int((~meta_rows).sum()), [c_new[k] for k in ("ptA", "ptB", "lcA", "lcB")]))
    # 2. only one sensor set: the flag of the other stays off, A = 1 and B = 0
    patch_cal_blocks(p, (False, True), [1.0, 0.0, vals[2], 0.0]); c, _ = read_cal(p)
    assert (not c["ptSet"]) and c["lcSet"] and c["ptA"] == 1.0 and c["ptB"] == 0.0 and c["rev"] == c_old["rev"] + 2
    # 3. a file with no block: one block is added after the last whole record, a partial record at the end stays at the end
    for label, extra in (("whole records only", b""), ("with a partial record at the end", b"")):
        q = os.path.join(tmp, "b.bin"); shutil.copy(without_blocks, q)
        orig = np.fromfile(q, np.uint8)
        if label.startswith("whole"):
            orig = orig[:orig.size // 8 * 8]; orig.tofile(q)           # cut the tail: a file of whole records
        size0 = orig.size; nr = size0 // 8; tail = orig[nr * 8:]
        assert not read_cal(q)[0]["valid"]
        n = patch_cal_blocks(q, (True, True), vals); c, blk = read_cal(q); out = np.fromfile(q, np.uint8)
        assert n == 1 and out.size == size0 + 48 and c["valid"] and c["rev"] == 1 and c["nBlocks"] == 1 and blk == [nr + 1]
        assert (out[:nr * 8] == orig[:nr * 8]).all() and (out[nr * 8 + 48:] == tail).all()
        assert [c[k] for k in ("ptA", "ptB", "lcA", "lcB")] == f32
        print("no block, %s (tail %d bytes): block at record %d, +48 bytes, records and tail unchanged" % (label, tail.size, nr + 1))
    # 4. a damaged block (bad CRC) is not touched, the valid ones are
    r = os.path.join(tmp, "c.bin"); shutil.copy(with_blocks, r)
    raw = np.fromfile(r, np.uint8); bad = blk_old[3]; raw[(bad + 2) * 8 + 4] ^= 0xFF   # kind 4 payload of block 4
    raw.tofile(r)
    c_dmg, blk_dmg = read_cal(r); assert len(blk_dmg) == len(blk_old) - 1
    snap = whole(r)[bad - 1:bad + 5].copy()
    patch_cal_blocks(r, (True, True), vals)
    assert (whole(r)[bad - 1:bad + 5] == snap).all()
    assert read_cal(r)[0]["nBlocks"] == len(blk_old) - 1
    print("damaged block left alone, the other %d rewritten" % (len(blk_old) - 1))
    shutil.rmtree(tmp); print("cal patch OK")

if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "test": test(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "show":
        for f in sys.argv[2:]: print(f, read_cal(f)[0])
    else: sys.exit(__doc__)
