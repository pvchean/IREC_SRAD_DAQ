# MTP Hardware Report

Author: MTP HARDWARE agent. Date: 2026-11-11. Branch `mtp-usb-mode`. Language: ASD-STE100. Labels: MTP-HW-n.
Zero NAND writes. No MTP command was sent. `LEAVE`, `REC ON`, `usbreset`, `mtp-detect` were not used.

## MTP-HW-1 Step 1: identify the running firmware

* Command: `python3 tools/mtp_spike/host_cdc.py /dev/ttyACM0 INFO 3`.
* Reply: `INFO: spike files mode, up 75 s, heap 351040`. This is the old spike firmware (v1 or v3: the reply does not say which). It has no NAND code.
* Result: OK.

## MTP-HW-2 Step 2: get daq_mtp_force on the board

* `tools/mtp_hosttest/run.sh`: 8427 checks passed, 0 failed.
* `pio run -e daq_mtp_force`: SUCCESS (RAM 64.7 %, flash 27.7 %). `nm -C .pio/build/daq_mtp_force/firmware.elf | grep tud_mtp_`: `T` for the 6 callbacks and `tud_mtp_data_*`/`response_send`.
* CDC `DL` was sent to the running spike firmware (`host_cdc.py /dev/ttyACM0 DL 2`).
  The port closed at once (pyserial: "device reports readiness to read but returned no data").
* Then the agent polled `lsusb | grep 303a` for about 15 s: NO 303a device appeared, no /dev/ttyACM*, no /dev/ttyUSB*.
* ANOMALY (as the task predicted for the old v1): the board is gone after `DL`. Rule applied: STOP, do nothing else.
* Result: FAIL. The board is invisible. It needs a human: hold BOOT, unplug and plug in the USB cable (full power loss first, wait 3 s),
  check `lsusb` shows `303a:1001`, then run `~/.platformio/penv/bin/pio run -e daq_mtp_force -t upload --upload-port /dev/ttyACM0`
  (the build is ready in `.pio/build/daq_mtp_force`), then unplug, wait 3 s, plug in WITHOUT BOOT.

## MTP-HW-3 Not done (blocked by MTP-HW-2)

Steps 3 to 8 (power-cycle proof, INFO/LOG DIAG, REBOOT, DL reflash, HANG, ptp_probe, libmtp) were not run.
Nothing of the real firmware has run on hardware yet.

## MTP-HW-4 Orchestrator run, 2026-11-11 08:20-08:33 (the user held BOOT for the first flash)
Board: DAQ with NAND (usable_blocks 1019), no SD card. Firmware: `daq_mtp_force` (commit of this branch at that time).
| Step | Result |
| :-- | :-- |
| BOOT flash of daq_mtp_force | PASS. `ttyACM0` appears about 1 s after `303a:1001`. |
| Power cycle | PASS. `303a:1001` for 10 s, then `303a:4041`. |
| CDC `INFO` | `mode=files rec=0 mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0` |
| CDC `LOG` | NAND mounted read only (Path A). DIAG: heap_free 56904, min 53624; free stack TinyUSB 6804, mtp_vfs 7500, cmd 6560, main 1456, led 2120. |
| `REBOOT` | PASS. 10 s `1001`, then `4041`, no replug. |
| `DL` and reflash | PASS. `1001` in about 1 s, flash with no BOOT, boots to files mode. |
| ptp_probe stages 1-3 | PASS (OpenSession 30 ms; one storage "DAQ", read-only, id 0x00010001; root has NAND and SD). |
| ptp_probe stage 6 (writes) | PASS. SendObjectInfo and DeleteObject return 0x200E, FormatStore 0x2005. Device stays alive. |
| libmtp `mtp-detect`, `mtp-folders`, `mtp-files` | PASS. 18 operations incl. GetPartialObject and GetPartialObject64. |
| Real file, stages 4-5 | PASS. `NAND_F00002D4.BIN`, 323696 bytes, CRC32 f3d49e4f twice; 20 random GetPartialObject reads match (lengths 1 to 65536, page crossings). Speed 767 KB/s. |
| libmtp `mtp-getfile` | PASS. Same size and CRC32 f3d49e4f. `bin_to_csv.py` decodes 40450 samples (PT sawtooth, 1 ms steps). |
Not tested: `HANG`, `LEAVE` (forbidden), SD card (not installed), Windows, macOS, a desktop file manager (gvfs/Dolphin), a transfer longer than 1 MB.
Note: `SendObjectInfo` refusal takes 2 s in the probe because the probe waits for an early response before it sends the data phase.

## MTP-HW-5 NAND write log (the user approved a 30 s test recording at 08:2x, limit for the task 500 Mbit)
* Why: the NAND had no files, so MTP reads could not be tested.
* How: env `daq_sim` (synthetic PT 1 kHz and load cell 333 Hz), `REC ON` for 29 s, `REC OFF`, `PFSFLUSH`. Then `daq_mtp_force` again.
* Result from `LIST`: file id 2, 173 pages, size 324548 bytes (323696 bytes of records). `ingested=323696 dropped=0`.
* Bytes written to the NAND: about 173 pages x 2048 B = 354 KB = 2.8 Mbit. That is 0.6 % of the 500 Mbit limit. The file stays on the NAND (no delete without a format).
* Total NAND writes of the MTP task: 2.8 Mbit (this run only). Before this run: 0.

## MTP-HW-6 Read-only soak, desktop file manager, concurrency, DIAG (2026-11-11 08:35-08:55)
Board: `daq_mtp_force`, files mode, `/dev/ttyACM0` (303a:4041). Zero NAND writes. No HANG, LEAVE, REC ON, DL or REBOOT was sent. Nothing was flashed.
The NAND holds one file: `NAND_F00002D4.BIN`, 323696 bytes, CRC32 f3d49e4f. Helper scripts are in the agent scratchpad (`soak.sh`, `poll.py`, `logtime.py`), not in the repository.

### MTP-HW-6.1 Desktop file manager path (task 1): NOT TESTABLE on this PC
* `gio mount -l` shows no mount. `gio list mtp://` returns "Operation not supported". The packages `gvfs` and `gvfs-mtp` are not installed.
* `jmtpfs` and `simple-mtpfs` are not installed. Nothing was installed.
* The KDE path exists (`kioclient` 6.8.2, `kio-extras` with the `kmtpd` module, Dolphin). `kioclient ls mtp:/` lists no device. `kmtpd` reports 0 devices (D-Bus `listDevices` returns an empty array).
* Cause on the PC (not in the firmware): `kmtpd` uses Solid `PortableMediaPlayer`. Solid needs the udev properties `ID_MTP_DEVICE` and `ID_MEDIA_PLAYER`. The udev database of the board has neither (`/run/udev/data/c189:62`). Solid shows interface 1-1:1.2 as "Camera".
* `/usr/lib/udev/mtp-probe` returns 1 for the board (run by hand, and the journal shows "was an MTP device" at 08:29:42, twice). `/dev/libmtp-1-1` exists. So the probe accepts the board, but the property is missing from the database. The reason is not found (probably the second udev event, "bind", rewrites the database entry; not proved).
* Result: the file copy through a desktop file manager is untested. A replug (new "add" event) may fix the property. The user must try this, or use Dolphin, after a replug.

### MTP-HW-6.2 Soak (task 2): PASS
| Item | Result |
| :-- | :-- |
| Probe `-s 5` (open, list, 2 full reads, 20 random reads, close), 30 runs, with the CDC port given to the probe | 30 of 30 PASS, 0 failures |
| Time per run | 7.57 to 7.59 s (most of it is the 0.7 s pauses of the probe between steps) |
| Full read speed (60 reads) | min 714.4 KB/s, max 717.6 KB/s. CRC32 f3d49e4f every time. |
| Slowest random read (65536 B) | 98.3 ms |
| `mtp-getfile 3`, 5 copies | 5 of 5: size 323696, CRC32 f3d49e4f. 0.85 to 0.88 s each (about 360 KB/s, includes session open and refresh). |
| CDC `INFO` after every run (35 times) | always `mode=files mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0` |
| Total time (30 runs, 5 copies, 35 INFO) | 339.6 s |
No error, no stall, no reconnect. The port stayed 303a:4041 all the time.

### MTP-HW-6.3 Concurrency: CDC INFO every 200 ms during an MTP read (task 3): PASS
* Method: one process sent `INFO` every 200 ms for 45 s (225 requests). Six `ptp_probe -s 4` runs (two full reads each) ran at the same time.
* INFO: 225 of 225 answered, all with blocked_writes=0 nor_blocked=0 lost_pages=0. Latency min 50.2, median 50.5, max 52.0 ms (this is the 50 ms read poll of the script, so the true latency is shorter). No INFO was late during a read.
* MTP: 6 of 6 PASS, CRC32 f3d49e4f. Speed 714.4 to 717.4 KB/s. Without INFO: 714.4 to 717.6 KB/s. No measurable change.
* Limit: INFO is a short reply. A large CDC stream (for example LOG during a read) was not tested at the same time.

### MTP-HW-6.4 LOG and the DIAG line (task 4)
* Result: the DIAG line is present. After the soak and after the concurrency test the LOG shows it as the last line before `END`.
`DIAG heap_free=56692 heap_min=49524 stack_free_bytes: TinyUSB=5540 mtp_vfs=5884 cmd=6304 main=1376 led=2120`
| Value | After boot (HW-4) | After soak and concurrency |
| :-- | :-- | :-- |
| heap_free | 56904 | 56692 |
| heap_min | 53624 | 49524 (about 4 KB used at the worst point during MTP) |
| TinyUSB task free stack (size 8192) | 6804 | 5540 |
| mtp_vfs free stack | 7500 | 5884 |
| cmd free stack | 6560 | 6304 |
| main free stack | 1456 | 1376 (the lowest margin; main stack is 3584 B per `usb_files.c`) |
| led free stack | 2120 | 2120 |
* Stack use is stable and small after 30+ sessions. The lowest margin is the main task (1376 bytes). It is not caused by MTP reads.
* Why the DIAG line was missing in the earlier read: it could not be reproduced. In 8 reads (delays 0 to 1.5 s before the host read) the full dump arrived: about 6300 bytes in 0.27 s, DIAG and `END` at the end. `ring_dump()` in `src/usb_files.c` always writes the ring first, then the DIAG line, then `END`. The ring (6144 B, `RING_SZ`) wraps without harm, and it holds only the last 45 lines because the MTP trace lines (`tinyusb_mtp_transport: MTP trace ...`) are at level INFO and fill it fast. DIAG is built after the ring, so a wrap cannot remove it.
* Most probable cause of the earlier miss: the reader stopped before the end (a 2 s limit with a slow line reader, or `head`/`tail` on the output), or the reply was mixed with the CDC INFO output of the probe. Not proved. Advice: read until the line `END`, not for a fixed time.
* Side note: the MTP trace at INFO level (every command, two lines each) uses the ring and the CDC time for no gain in a flight build. Lower the level for the release build (not changed here).

### MTP-HW-6.5 Problems and open items
* No problem with MTP, CDC or the board in any test. No anomaly. The board is still in files mode, mounted read only.
* Open: the desktop file manager (MTP-HW-6.1). Windows and macOS. HANG (crash recovery). SD card. `main` stack margin 1376 bytes.
