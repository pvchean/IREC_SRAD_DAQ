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

## MTP-HW-7 Polished build and KDE file-manager path
### MTP-HW-7.1 Polish verification (build 83c2640, daq_mtp_force)
* Flash by `DL` (no BOOT button): OK. Board boots to files mode (MTP + CDC).
* `INFO`: mode=files mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0.
* DIAG: `heap_free=54516 heap_min=51576`. Free stack: TinyUSB=6628 mtp_vfs=5916 cmd=6560 main=3408 led=2120. The main task has 2032 B more margin than before (1376 to 3408).
* The `MTP trace` lines are gone from the LOG ring. The ring now shows only boot lines, the refresh lines and real events.
* The raw probe (5 runs) and `mtp-files` were blocked by `kiod6` (KDE kmtpd holds the MTP interface; LIBUSB_ERROR_BUSY). This is a host-side cause. They were not repeated on the polished build.

### MTP-HW-7.2 KDE path (`kioclient5`, the desktop file-manager test): PASS
* Device URL: `mtp:/IREC DAQ`. Storage: `DAQ`. Folders: `NAND`, `SD`. `NAND` holds `NAND_F00002D4.BIN`. `SD` is empty (no card).
* Copy of `NAND_F00002D4.BIN` to a local folder, 3 runs: 0.60, 0.58, 0.61 s (about 540 KB/s with the KDE start-up cost). Each copy: 323696 B, CRC32 f3d49e4f.
* Write test: `kioclient5 copy` of a 3 B local file into `NAND`: refused. KDE said "There is not enough space on the disk" (the storage reports free=0). The file list did not change.
* Delete test: `kioclient5 rm` of the NAND file: refused ("Could not delete file"). The LOG shows `write operation 0x100b refused (read-only)`. The file list did not change.
* `INFO` after every action (3 copies, write, delete): mode=files blocked_writes=0 nor_blocked=0 lost_pages=0. The refusals are counted by the MTP layer, not by the NAND layer, so blocked_writes stays 0.
* LOG after the KDE session (read to `END`): no stall, no protocol error, no reconnect. Only the expected refusal line. The SD errors (`sdmmc_init_ocr 0x107`) are from the missing card at OpenSession.
* Not tested: Windows, macOS, HANG, SD card, Dolphin GUI itself.

## MTP-HW-8 Crash recovery test: CDC `HANG` (2026-11-11 09:23 to 09:31, user at the PC)
Verdict: PASS. 5 of 5 crash cycles recovered with no human, no replug and no stale device. Risk MTP-V-1 did NOT appear on this PC.
Build: `daq_mtp_force` (commit 333c862 code, files mode, MTP + CDC). NAND: one synthetic file `NAND_F00002D4.BIN`, 323696 B, CRC32 f3d49e4f. NAND writes: zero.
`HANG` blocks the TinyUSB task. The task watchdog (8 s, panic, silent reboot) must reset the chip. Host: Linux 7.2.11, xHCI, kiod6 (KDE) holds the MTP interface.
Method: `lsusb` and `/dev/ttyACM*` every 0.5 s from 5 s before to 45 s after `HANG`. Kernel times come from `journalctl -k`. Times are seconds after the `HANG` line.

### MTP-HW-8.1 T1: HANG while idle (3 cycles)
| Cycle | Host sees `USB disconnect` | New 303a:1001 | 1001 -> 4041 (second disconnect) | New 303a:4041 + ttyACM | CDC after |
| :-- | :-- | :-- | :-- | :-- | :-- |
| 1 | about 8.7 (between samples +8.70 and +9.22) | about 9.1 | about 20.2 | about 20.4 (09:23:59.82) | INFO and LOG OK |
| 2 (HANG 09:25:02.675) | 8.72 | 9.13 | 20.07 | 20.44 | INFO and LOG OK |
| 3 (HANG 09:26:10.887) | 8.72 | 9.14 | 20.07 | 20.46 | INFO and LOG OK |
* The host saw a real disconnect each time. The device gets a new address each time. The old address never stays: there is no stale 4041. The board is invisible for less than 0.3 s.
* In the 11 s window (1001) the grace countdown runs. Then TinyUSB starts and 4041 appears. Total time with no MTP: about 20.5 s.
* LOG after each crash (read to END): `usb_files: reset reason 6, USJ pads were clean` (6 = `ESP_RST_TASK_WDT`). Then `NAND mounted read only: refused writes nand+nor=0, NOR refused=0`, then `USB MTP starts in 10 s` to `1 s`, then `files mode running (MTP + CDC)`.
* DIAG after the cycles: `heap_free=54516 heap_min=51576` (same as before the test). Free stack: TinyUSB=6628 mtp_vfs=5916 cmd=6560 main=3408 led=2120.
* INFO after each cycle: `mode=files rec=0 mounted=1 index=1 blocked_writes=0 nor_blocked=0 lost_pages=0`. The read-only PFS mount works again after each crash. The file is still listed (`refresh NAND: store says 1, list has 1`).
* DL and flash after the crash cycles: CDC `DL` gave 303a:1001 in about 1.5 s. `pio run -e daq_mtp_force -t upload --upload-port /dev/ttyACM0` wrote 1162640 B, hash verified, board back as 4041 about 12 s after the flash ("reset reason 11" = USB download reset). The way back with no BOOT button still works after crash resets.
* Kernel log (09:23 to 09:31): only the expected `USB disconnect` / `new full-speed USB device` / `New USB device found` lines. No xhci error, no `device not accepting address`, no `reset`, no `cdc_acm` error.

### MTP-HW-8.2 T2: HANG during an MTP read (2 cycles)
Reader: `kioclient5 copy` of the NAND file in a loop (0.6 s per copy, 0.05 s gap). `HANG` was sent while a copy was running (the copy started 0.25 to 0.37 s before `HANG`).
| Cycle | Reader result | Host disconnect | 1001 | 4041 | First good read after |
| :-- | :-- | :-- | :-- | :-- | :-- |
| 1 (HANG 09:28:59.905) | The running copy stalled 8.4 s, then failed: `Could not read file` with 180276 B written (partial) | 8.72 | 9.12 | 20.43 | at 20.5 (about 0.1 s after 4041): CRC f3d49e4f, 323696 B |
| 2 (HANG 09:30:11.404) | Same: stalled, failed with 49204 B written (partial) | 8.72 | 9.14 | 20.46 | at about 20.5: CRC f3d49e4f |
* The reader does NOT time out by itself: it waits until the host sees the disconnect (about 8.7 s), then gets an error. The partial file is left on disk. An application must delete it (its CRC differs).
* From the disconnect to 4041 the reader fails fast, about 0.14 s per try (61 failed tries per cycle: no MTP device, or only the 1001 serial device).
* kiod6 found the new MTP device by itself. The first copy after 4041 worked with no restart of KDE and no user action. All successful reads of both cycles (56 and 56 copies) have CRC f3d49e4f. A final read after the tests: CRC f3d49e4f.
* Host bus health: `lsusb` listed all other devices (HDMI card, fingerprint, Bluetooth, camera) in every sample. The line count was constant (13), 12 only in the 0.5 s gap when the board was off the bus. No xhci error in the kernel log.
* After the cycles: INFO `blocked_writes=0 nor_blocked=0 lost_pages=0 mounted=1`. LOG: `reset reason 6`, DIAG `heap_free=54452 heap_min=47476`, TinyUSB stack free 5796 (the lowest value seen, because it ran the read).

### MTP-HW-8.3 T3 (HANG during OpenSession): not done
The OpenSession refresh lasts about 0.05 s. A timed hit is not repeatable with these tools. T2 shows the same host path (a blocked USB task in a session).

### MTP-HW-8.4 Verdict and risk for the DAQ
* PASS: no human needed in 5 of 5 cycles (idle and during a read). Recovery is automatic: reset 8.7 s after the hang, 1001 for 11 s (flash window), 4041 after about 20.5 s in total.
* MTP-V-1 (stale device after a crash): not reproduced. The host saw a disconnect at the reset each time. Reason not proven. Likely the reset or the bootloader drops D+ for a short time. The fix MTP-V-1.3 is NOT needed on this evidence. Do not apply it now: it changes crash-path code and needs its own hardware proof.
* What a hang costs in files mode: about 20.5 s with no USB. An MTP copy in progress fails (partial file); the user must start it again. No NAND data is touched (read only, `blocked_writes=0`).
* Not proven: (1) other crash types (panic by abort, stack overflow, interrupt watchdog): only the task-watchdog path was tested, because only `HANG` exists in this build. (2) Other hosts: Windows, macOS, a USB hub, a different xHCI chip may keep a stale device. (3) A hang in serial/recording mode (`daq`): HANG is not in that build, and the recovery there is not tested. (4) A hang in the middle of a bulk transfer on the host side was tested only with kioclient5/libmtp-like KDE access, not with the raw `ptp_probe`.
* Files: timelines and read logs are in the scratchpad (`hang/t1a..t2b.timeline`, `*.reads`), not committed.
