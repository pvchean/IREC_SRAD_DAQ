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

## MTP-HW-9 Switch test, step 1: env `daq_mtp` with MODE2 (GPIO 2) in the FILES position (2026-11-11 09:33, user at the PC)
Verdict: PASS. The board has a working MODE2 switch now. With the switch low, `daq_mtp` (no force) boots to files mode. NAND writes: zero. No `LEAVE`, no `REC ON`, no `daq` or `daq_sim` flash.
The earlier note "GPIO 2 reads high, no switch" is out of date. Evidence: `daq_mtp` has no `DAQ_MTP_FORCE_FILES`, so `io_init()` sets files mode only when `gpio_get_level(MODE2_PIN) == 0`. The LOG ring starts after `io_init()`, so the `mode2=files` line is not in it. The proof is the behaviour (files mode, no "files mode skipped once" line).

### MTP-HW-9.1 Build checks (`pio run -e daq_mtp`)
* Build: SUCCESS, no warning. RAM 64.7 %, flash 27.7 % (1162189 B).
* `nm`: `T` for the six `tud_mtp_*_cb` (command_received, data_complete, data_xfer, request_cancel, request_device_reset, request_get_device_status). Three optional callbacks stay `W` (normal).
* The string `HANG` is absent from the `daq_mtp` image (count 0). It is present in the `daq_mtp_force` image (count 1), as designed.
* `tools/mtp_hosttest/run.sh`: 8427 checks passed, 0 failed.

### MTP-HW-9.2 Flash and boot
* Before the flash, the force build answered `INFO`: `mode=files rec=0 mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0`. LOG: `reset reason 6` (old HANG test), DIAG main stack 3408.
* CDC `DL`: `303a:1001` and `/dev/ttyACM0` in about 1 s. Flash `-e daq_mtp -t upload --upload-port /dev/ttyACM0`: 1162624 B, hash verified.
* Bus watch after the flash (switch low): `303a:1001` for the grace window, then the board left the bus, then `303a:4041` plus `/dev/ttyACM0` at about 12 s after the reset. This is the same sequence as `daq_mtp_force`.
* CDC `INFO`: `mode=files rec=0 mounted=1 index=1 blocked_writes=0 nor_blocked=0 lost_pages=0`.
* CDC `LOG` (read to `END`): `reset reason 11` (USB download reset, because the flash used `DL`), `FILES MODE (read only): no recording, no sensors, no WiFi`, `NAND mounted read only: refused writes nand+nor=0`, the 10 s countdown, `files mode running (MTP + CDC)`, the SD error from the missing card. DIAG: `heap_free=54516 heap_min=51576`, free stack TinyUSB=6628 mtp_vfs=5916 cmd=6560 main=3424 led=2120.

### MTP-HW-9.3 MTP checks (kioclient5, kiod6 holds the interface)
* `mtp:/IREC DAQ/DAQ` lists `NAND` and `SD`.
* 3 copies of `NAND_F00002D4.BIN`: 0.57, 0.60, 0.59 s. Each 323696 B, CRC32 f3d49e4f.
* `INFO` after the copies: `blocked_writes=0 nor_blocked=0 lost_pages=0`. LOG: no new error.
* CDC `HANG` in this image: reply `ERR not available in files mode (read-only)`. The board stayed on the bus (`4041`), `INFO` clean after. The HANG test command is not in `daq_mtp`.
* Not run: `ptp_probe` (kiod6 holds the interface), write refusal test (done in MTP-HW-7).

### MTP-HW-9.4 Check list for the user's flip to the SERIAL position (MODE2 high)
This build in serial mode is the full DAQ firmware: sensors, chain, `pfs_writer`, read-write PFS mount, WiFi/telemetry as in `daq`. Recording is locked (`REC ON` refused, MODE1 does not auto-record). No `LEAVE` and no `REC ON`.
1. Flip the switch, then do a full power cycle (unplug 3 s, plug in). A flip alone does not change the mode: `io_init()` reads GPIO 2 at boot only.
2. `lsusb`: only `303a:1001` (USB Serial/JTAG). It must stay. No `303a:4041` may appear, now or after 30 s. `/dev/ttyACM0` must exist.
3. Serial: open the port at 115200. The DAQ banner and the normal commands must answer (`PING` gives `DAQBOARD V1`). No "FILES MODE" line.
4. `INFO`: must show `mode=serial` (not `mode=files`), and rec locked (rec=0). Read it only. `REC ON` must NOT be sent as a test of the lock unless the user agrees (NAND write risk). Check that the sensors run (`STATUS`, `STREAM` if the user wants) and that the LED shows normal DAQ states.
5. No TinyUSB: the LOG must not show `tinyusb_mtp`, `TinyUSB Driver installed`, or `usb_files: files mode running`. `usb_files_early_init` still runs (reset reason line, "USJ pads were clean").
6. MODE1: with MODE1 high at boot the board must NOT record (`io_rec_disable()`: log line `daq_mtp build: recording is disabled`). The AUTOREC LED must stay off.
7. Watchdog: `daq_mtp.defaults` sets TWDT 8 s, panic, silent reboot for both modes. In serial mode a blocked task would reset the board with no message (the log shows `reset reason 6` after the reboot). Watch 2 minutes of normal run: there must be no unexpected reboot (the USB 1001 device must not disconnect). `HANG` does not exist in this image, so the watchdog cannot be forced. Debug serial mode with `daq`, not `daq_mtp`.
8. Return to files mode: flip the switch to low and power cycle (or CDC/serial `DL` and a flash). Expected: 1001 for about 10 s, then 4041.

## MTP-HW-10 Different board: identify, flash `daq_mtp`, files mode with MODE2 low (2026-11-12 14:2x, user at the PC)
Verdict: PASS for files mode on a second board. This board has an EMPTY NAND (no file). NAND writes: zero. No `REC ON`, no `LEAVE`, no `daq` flash, no `usbreset`, no `sudo`. The serial-position check (MTP-HW-9.4) is NOT done yet.

### MTP-HW-10.1 Board identification
* Serial/JTAG serial number (`lsusb -v`): `28:84:85:46:83:04` (MAC 28:84:85:46:83:04). In files mode, TinyUSB string `288485468304`. The spike board in Spike_Report.md had `2884854684E0`: this is a different board.
* Chip rev v0.2. NAND: `usable_blocks=1018 retired=6` (the first board: 1019).
* Firmware found (before the flash): plain `daq` build, version `fa2d136-dirty`, compile time Oct 30 2026 20:56. It ran in serial style (`303a:1001` for 20+ s) with MODE2 low, because plain `daq` ignores MODE2. Boot line: `io: mode1=manual mode2=files addr=2`. `STATUS:OK rec=0 auto=0 mounted=1 file=2 usable_blocks=1018 sd=0`. No ADS1115 (0x48), no NAU7802 (0x2A) answer. SD init failed every 2 s (no card). No recording was running, so `REC OFF` was not sent.
* WARNING for the serial port: on this plain `daq` firmware, opening `/dev/ttyACM0` resets the board (USB Serial/JTAG DTR/RTS). Every open gave a fresh boot (`up_s=1`). With MODE1 high this would start a recording. MODE1 was manual here.

### MTP-HW-10.2 Build and flash
* Commit: `358a709`. `pio run -e daq_mtp`: SUCCESS, RAM 64.7 %, flash 27.7 % (1162189 B). `nm`: `T` for the six `tud_mtp_*_cb`. String `HANG`: count 0. `tools/mtp_hosttest/run.sh`: 8427 passed, 0 failed.
* Flash: `pio run -e daq_mtp -t upload --upload-port /dev/ttyACM0` (serial mode, auto reset): 1162624 B, `Hash of data verified`.
* Bus after the flash: `303a:1001` for about 6 s, then `303a:4041` (IREC DAQ). It stayed 4041 for 35 s and again about 60 s later. Files mode, as expected.

### MTP-HW-10.3 Files-mode checks
* IMPORTANT: the TinyUSB CDC answers only when the host sets DTR. With `dtr=False` there is NO reply to any command (INFO, PING). Use `serial.Serial(...); s.dtr=True`. The same tool works with `dtr=False` in serial mode.
* `INFO`: `STATUS:OK mode=files rec=0 mounted=1 index=1 blocked_writes=0 nor_blocked=0 lost_pages=0` (also after 60 s).
* `PING`: `01010101 10101010 DAQBOARD V1 01010101 10101010`. This is what MATLAB needs.
* `MATLAB PING`: `ERR not available in files mode (read-only)`. If a MATLAB app sends the text `MATLAB PING` (not `PING`) it gets no `DAQBOARD V1` in files mode. Check which command the apps send.
* `LOG` to `END`: reset reason 11, `FILES MODE (read only)`, `NAND mounted read only: refused writes nand+nor=0`, 10 s countdown, TinyUSB installed, `files mode running (MTP + CDC)`, `refresh NAND: store says 0, list has 0`, the SD error (no card). DIAG: `heap_free=54516 heap_min=51576`, free stack TinyUSB=6628 mtp_vfs=5916 cmd=6560 main=3488 led=2120.
* `LIST`: `LIST:0 files`. `FILES`: empty. `CHECK` gives `ERR no file 2`. `SDLS`: `ERR no card`. So this NAND is empty: no file to copy, no CRC test. The 3-copy test was NOT run.
* `kioclient5 ls "mtp:/IREC DAQ/DAQ"` lists `NAND` and `SD`; `NAND` is empty. `ptp_probe` not run: `kiod6` holds the interface.
* Not tested: a copy of a real file on this board (needs a recording: user decision), serial position (MTP-HW-9.4), 3-minute run (only about 2 minutes of files-mode run seen, no reset).

## MTP-HW-11 Serial position (MODE2 high): `daq_mtp` on board 28:84:85:46:83:04 (2026-11-12 14:39 to 14:46, user at the PC)
Verdict: PASS. The safety feature "MODE2 serial = MTP disabled" works. The board stayed on `303a:1001` and no MTP interface appeared. Test only: no build, no flash, no `usbreset`, no `sudo`. Commands sent: `INFO`, `PING`, `LIST`, `FILES`, `STATUS`. Not sent: `REC ON`, `LEAVE`, `DL`, `REBOOT`, `MATLAB PING`. NAND writes: zero. Setup: no SD card, MODE1 manual, no 5 V supply (ADS1115 and NAU7802 absent).

### MTP-HW-11.1 Results
| # | Check | Result |
|---|---|---|
| 1 | `lsusb \| grep 303a`, 1 sample per second, 40 s (14:39:26 to 14:40:07) | Only `303a:1001`, Device 090, in all 40 samples. No `303a:4041`. |
| 1 | Same check about 6 minutes after the plug-in (14:45:44) | Only `303a:1001`. `grep -c 4041` gives 0. |
| 1 | `lsusb -v -d 303a:1001`, serial `28:84:85:46:83:04` | 3 interfaces: CDC control (class 2), CDC data (class 10), vendor 255/255/1 (the JTAG interface). No MTP/PTP interface (no class 6). |
| 2 | Passive read 10 s, `dtr=False rts=False` set before open | The board RESET at the open (ROM boot lines and `entry 0x403c886c` seen at 226 ms). Setting DTR/RTS low does not stop it. See MTP-HW-11.3. |
| 2 | Boot text | Normal ESP-IDF log. `io: mode1=manual mode2=serial addr=2`, `daq_mtp build: recording is disabled`, `chain on UART1/UART2 at 460800 baud`, ADS1115 and NAU7802 warning (one line each), `pfs: Mount complete (Path A)`, then the SD error. App `f574447-dirty`, compile time Nov 11 09:01:37 (see MTP-HW-11.3). |
| 3 | `INFO` (up_s=2 after the open reset) | `STATUS:OK rec=0 auto=0 addr=2 mounted=1 path=1 index=1 file=2 ingested=0 dropped=0 pages=0 degraded=0 no_storage=0 nor_failed=0 verify_fail=0 rs_repairs=0 usable_blocks=1018 retired=6 sd=0 sd_bytes=0 sd_drop=0 sd_err=0 up_s=2`, then `CHAIN: tx=3 rx1=0/0 rx2=0/0 ... fwd=0 tx_drop=0`, `LC: running=0 fault=0 samples=0 i2c_err=0 backup=0 restarts=0`, `CAL: ... flash_ok=1`. |
| 3 | `PING` | `01010101 10101010 DAQBOARD V1 01010101 10101010` |
| 3 | `LIST` / `FILES` | `LIST:0 files` `LIST:END` / `FILES:BEGIN` `FILES:END` (NAND empty) |
| 3 | `STATUS` | `STATUS:OK (SD Overflows: 0)` |
| 4 | `INFO` every 30 s, 200 s total | up_s = 2, 29, 59, 89, 119, 150, 179: grows by 30 each time. No reset. `rec=0 auto=0 mounted=1` in all. `CHAIN tx` grows 1 per second (3, 30, 60, ..., 180 by the log). `LC samples=0 i2c_err=0 restarts=0` in all. |
| 4 | WDT, panic, reboot text | None. No `Guru`, `abort`, `rst:`, `reset reason`, watchdog line after the boot. USB device 090 never left the bus. |
| 4 | Log rate | About 250 bytes every 2 s (125 B/s), all from the SD retry (MTP-HW-11.3), plus one 64-byte `daq: in=0 drop=0 pages=0 ...` line every 10 s. 34.8 KB in 201 s including the boot text. Not a flood. |
| 6 | TinyUSB / MTP lines | None. `grep -i -E 'tinyusb\|mtp'` in the log: no match. No `files mode` line. |
| 6 | Heap | Not shown in serial mode (the DIAG line is in files mode only). Only the boot `heap_init` lines. |

### MTP-HW-11.2 Why a recording cannot start in this build in serial mode (code read)
* `src/io.c`: with `DAQ_MTP`, `io_init()` calls `io_rec_disable()` after it reads MODE1. `io_rec_disable()` sets `s_rec_locked = true`, `s_auto = false`, `s_rec = false`, and removes the MODE1 interrupt handler (`gpio_isr_handler_remove`). `io_init()` also removes it at its end when locked. `src/main.c` calls `io_rec_disable()` a second time at line 239.
* Auto record (MODE1 high): `s_rec = s_auto` is set first, but `io_rec_disable()` clears both right after. The board log shows `daq_mtp build: recording is disabled`. The button interrupt that toggles `s_rec` is removed. The AUTOREC LED follows `s_auto`, so it stays off.
* Manual record: `rec_set(on)` is `if (!on || !s_rec_locked) s_rec = on;`. It cannot set `s_rec` to true when locked. In addition `src/cmd.c` line 674: `REC ON` while `rec_locked()` prints `REC:ERR recording is disabled in this build (daq_mtp)` and does not call `rec_set`.
* The recording state `rec_get()` is the only gate for the data path (`src/chain.c` line 162, `src/main.c`). It stays false, so nothing goes into the PFS writer. This matches the board: `ingested=0 pages=0`, `rec=0 auto=0`.
* Not covered by the lock: `PFSFLUSH`, `FORMAT`-type commands and `CAL` still exist in serial mode (the PFS is read-write). The user rule is: never send them in this build. `CAL` and `PFSFLUSH` were not sent.

### MTP-HW-11.3 Odd points
1. RESET AT PORT OPEN (also in serial mode, also with `dtr=False rts=False`). Each open of `/dev/ttyACM0` gave a fresh boot (up_s=2 in the first `INFO`). This is the USB Serial/JTAG auto-reset on the DTR/RTS edges that the Linux driver makes at open. Setting the lines in pyserial before `open()` does not help, because the kernel raises them at open. Result: a serial session always restarts the board. With MODE1 high this would start a recording in the plain `daq` build. In `daq_mtp` it cannot (MTP-HW-11.2). The 200 s uptime above is from the last open, not from the plug-in. The board had been on the bus since the replug and showed no disconnect.
2. App version string `f574447-dirty`, compile time Nov 11 09:01:37. The board has the `daq_mtp` behaviour (the lock line, MODE2 read). The flash in MTP-HW-10 was `358a709` built on Nov 12. The version and time are likely stale because `esp_app_desc` is not rebuilt by an incremental `pio` build. Not proved. Do not use these two fields to identify a build.
3. SD retry. With no card, the SD mount retries every 2 s and prints 3 error lines each time (`sdmmc_init_ocr ... 0x107`, `sdmmc_card_init failed`, `esp_vfs_fat_sdmmc_sdcard_init failed`). It never stops. It costs about 125 B/s of log and a few ms of SPI/SDMMC use every 2 s. It is a retry loop, not a hang. In `daq`, the same happens (MTP-HW-10.1).
4. Absent ADS1115 / NAU7802: ONE warning line each at boot (`trying again each second`). The retries are silent. No error flood, no reboot, no hang in 200 s. `LC: running=0 fault=0 samples=0 i2c_err=0 restarts=0` stayed constant. The I2C retry once per second is a low hang risk. Not stressed beyond 200 s.
5. `INFO` is not the same text as in files mode: no `mode=` field. In serial mode the mode shows only in the boot line (`mode2=serial`) and by `303a:1001` on the bus. A check for `mode=serial` in `INFO` (MTP-HW-9.4 item 4) cannot pass. Use the boot line.
6. Not tested: 5 V sensors running, `STREAM`, the LED states (nobody looked), WiFi/telemetry (not configured on this board), runs longer than 200 s after the last reset, `LEAVE`.

### MTP-HW-11.4 Verdict on "MODE2 serial = MTP disabled": PASS
The board in the serial position boots the DAQ, never starts TinyUSB, stays on `303a:1001` for more than 6 minutes, and has no MTP interface. Recording is locked by code and by the board log. Absent sensors and a missing SD card give warnings and silent retries only.
Next: flip MODE2 to the files position, power cycle, then the crash tests.

## MTP-HW-12 One small recording for the files-mode test, board 28:84:85:46:83:04 (2026-11-12 about 14:50, user at the PC)
Verdict: PASS. The NAND now holds one file. The board is back on `daq_mtp` in the serial position. The user allowed up to 1 Mbit of recording on this board.

### MTP-HW-12.1 Builds
* Commit `27bc4e5` (includes MTP-A). `pio run -e daq_sim` and `pio run -e daq_mtp`: SUCCESS. `nm`: `T` for the six `tud_mtp_*_cb`. `tools/mtp_hosttest/run.sh`: 8427 checks passed, 0 failed.

### MTP-HW-12.2 Recording log
| Item | Value |
|---|---|
| Date | 2026-11-12, about 14:50 |
| Board serial | 28:84:85:46:83:04 (no SD card, no 5 V, no real sensors, MODE1 manual) |
| Test | `daq_sim` (synthetic PT and load cell). One serial session, one port open. Boot wait 13 s. `INFO` and `LIST` before: `rec=0 auto=0 mounted=1`, 0 files. `REC ON`, 7.011 s (timer), `REC OFF`, `PFSFLUSH` (`PFSFLUSH:OK`), `LIST`. |
| File ID | 2 |
| Pages (from `LIST`) | 44 (limit was 55) |
| Bytes (from `LIST`) | 82544 |
| Mbit written | 44 x 2048 x 8 / 1e6 = 0.721 Mbit (limit 1 Mbit) |
| Ingested / dropped | NOT recorded. The `STATUS` line of the `INFO` after the recording was removed by an output filter in the test script. The port was not opened again, because each open resets the board and clears the counters. The first `INFO` after the next boot shows `file=3 ingested=0 dropped=0`, which is the new boot, not the recording. |
* Total NAND writes of this task: 0.721 Mbit. The file stays on the NAND (no delete without a format).
* After `daq_mtp` boots, `LIST` shows `FILE id=2 pages=44 size=82544`. The next file ID is 3.

### MTP-HW-12.3 Flash steps
1. `pio run -e daq_sim -t upload --upload-port /dev/ttyACM0`: serial mode, USB Serial/JTAG auto reset worked. 1047392 B, `Hash of data verified`.
2. Record (MTP-HW-12.2). Opening the port resets the board, so the script opened it once.
3. `pio run -e daq_mtp -t upload --upload-port /dev/ttyACM0`: 1162880 B at 0x10000, `Hash of data verified` (bootloader and partition table also verified).
4. 15 s after the flash: boot line `mode1=manual mode2=serial`, `daq_mtp build: recording is disabled`, `pfs: Mount complete (Path A)`. `INFO`: `rec=0 auto=0 mounted=1 file=3 ingested=0 dropped=0 pages=0`. `lsusb`: only `303a:1001`, no `303a:4041`. `REC ON` was not sent.

### MTP-HW-12.4 State of the board now
* Firmware: `daq_mtp` built at commit `27bc4e5`. MODE2 switch: SERIAL position. NAND: one file (id 2, 44 pages, 82544 bytes). SD: none.
* Next: the user flips MODE2 to FILES and power cycles. The files-mode copy and CRC test can now run on file 2.

## MTP-HW-13 Files mode with the MATLAB serial commands and a real file, board 28:84:85:46:83:04 (2026-11-12 14:57 to 15:04, user at the PC)
Verdict: PASS. In files mode the serial port does everything the MATLAB "Files & Plot" download needs: `MATLAB PING`, `STREAM OFF`, `FILES`, `CHUNK`. The serial download of file 2 and three MTP copies are byte-identical. NAND writes: zero. Firmware: `daq_mtp` at commit `27bc4e5` (MTP-A), MODE2 low, no SD card, no 5 V. Test only: no build, no flash, no `usbreset`, no `sudo`. Not sent: `REC ON`, `LEAVE`, `DL`, `REBOOT`, `STREAM ON` (sent once, refused). The MATLAB app itself was NOT run: the client is my own Python script with the same commands.
Scripts (not deliverables): `cli.py`, `dl.py` in the scratchpad (`.../scratchpad/hw13/`). Every serial script sets `dtr=True` (MTP-HW-10.3). The TinyUSB CDC port is `/dev/ttyACM0`. Opening it does NOT reset the board in files mode.

### MTP-HW-13.1 Test 1: bus timeline
* First attempt (14:57 to 14:58, 66 s): only `303a:1001`, Device 090, node created 14:50:04. The user had not replugged yet. I stopped and reported (no command sent).
* After the replug: `/dev/ttyACM0` created 14:59:35. At 15:00:08 the bus showed only `303a:4041` (Device 092, "IREC DAQ"). The 1001 grace window itself was NOT observed with a timestamp: the coordinator told me about the replug after it happened. The LOG shows the 10 s countdown and `MTP driver installed` at 12.0 s, so the order was 1001, then 4041.
* `303a:4041` stayed on the bus until 15:03 (after all tests). Same Device 092. No disconnect.
* CDC port: answers with `dtr=True`.

### MTP-HW-13.2 Test 2: INFO and LOG
* `INFO`: `STATUS:OK mode=files rec=0 mounted=1 index=1 blocked_writes=0 nor_blocked=0 lost_pages=0`.
* `LOG` to `END`: `reset reason 1, USJ pads were clean` (power-on), `FILES MODE (read only): no recording, no sensors, no WiFi`, `pfs: Mount complete (Path A)`, `NAND mounted read only: refused writes nand+nor=0, NOR refused=0`, 10 s countdown, `MTP driver installed`, `MTP storage registered: id=0x00010001 path=/daq name=DAQ total=122945536 free=0`, `files mode running (MTP + CDC)`, `refresh NAND: store says 1, list has 1`, then the SD error lines (no card).
* DIAG at the first read: `heap_free=54516 heap_min=51576`, stack free TinyUSB=6628 mtp_vfs=5916 cmd=6560 main=3376 led=2120.

### MTP-HW-13.3 Test 3: MATLAB compatibility (exact replies)
| Command | Reply |
|---|---|
| `PING` | `01010101 10101010 DAQBOARD V1 01010101 10101010` |
| `MATLAB PING` | The same banner line. Then 5 s with no byte at all (0 bytes, no sample lines): no stream started. |
| `STREAM OFF` | `STREAM:OFF` |
| `LIST` | `LIST:1 files`, `FILE id=2 flags=0x0000 pages=44 first_offset=0 size=82544`, `LIST:END` |
| `FILES` | `FILES:BEGIN`, `FILE PFS 2 82544 0000 0`, `FILES:END` (the MATLAB parser reads `bytes=82544`, `start=0`) |
| `CHECK 2` | `CHECK:OK file=2 records=7613 seq_breaks=0 missing=0 time_back=0 read_errors=0 seq=15467..23079 span_us=7611999 mean_interval_us=999 max_gap_us=1473 ...`; `CHECK:LC records=2538 seq_breaks=0 missing=0 time_back=0 span_us=7610994 mean_interval_us=2999 max_gap_us=3419 ...`; `CHECK:CAL meta_records=6 blocks=1 rev=0 ...` |
| `STREAM ON`, `DAQS`, `WIFI GET` | `ERR not available in files mode (read-only)` (each) |
| `CHECK 2 x` (bad argument, my typo) | `CHECK:WARN file=2 records=0 ...` (the argument parser reads `x` as a file id; harmless, no crash) |

Download as the MATLAB app does it (`dl.py`): `STREAM OFF`, wait `STREAM:OFF`, `FILES`, then `CHUNK PFS 2 <off> 16384` with off from `start` (0), reply `CHUNK:<off> <n> <crc32 hex>` plus n raw bytes, until n = 0.
* Run A (with `DAQS` and `WIFI GET` sent right after the `CHUNK` reply of chunk 0 and of chunk 2, before the next `CHUNK`): 6 requests, chunk sizes 16384 x 4, 15720, 0. Every chunk CRC matched. Total 81256 bytes in 0.11 s. Whole-file CRC32 `5e8cee05`. The refusal lines were skipped by the line search, as the MATLAB `waitLine` does. The download succeeded. `INFO` after: blocked_writes=0 nor_blocked=0 lost_pages=0.
* Run B (no injection): identical bytes and CRCs. Run C (chunk 4096): identical file. `cmp` of A, B, C: equal.
* Chunk CRCs: D090F9F9, C1E3E811, 95B1BBD8, 277DC419, 376059D3.
* SIZE DIFFERENCE: `LIST` and `FILES` say 82544 B, but `CHUNK` ends at 81256 B (10157 records x 8 = 81256 = 7613 PT + 2538 LC + 6 META). The 1288 B difference is 161 records. The MATLAB app uses `bytes` only for the progress bar (`approx`), and it stops at the n = 0 chunk, so it is not a problem. The MTP object size is 81256 (see 13.4). The `FILES` size is likely the raw page payload including the partial last page: not checked in the code.
* `daq_get.py` cannot run in files mode: its `daqs()` waits for `DAQS:` and `DAQS` is refused, so it ends with "no reply". It was not run. It only uses `STREAM OFF`, `DAQS`, `FILES`, `CHUNK`. The `CHUNK` part is the same as `dl.py`.
* Decode: `python3 -I tools/bin_to_csv.py dl_plain.bin` gives 10151 samples, calibration rev 0. PT: 7613 samples, interval 526 to 1473 us (mean 999.9999 us), raw count = seq = 15467 .. 23079, step +1 on all 7612 steps, no reset (a ramp over 7.6 s: the sawtooth does not wrap in this recording). LC: 2538 samples, mean interval 2999.998 us (max 3419), raw -2091996 .. -2089459. This matches `CHECK`.

### MTP-HW-13.4 Test 4: MTP (kioclient5)
* `kioclient5 ls "mtp:/IREC DAQ/DAQ/NAND"`: `NAND_F00002D2.BIN`. `SD` is empty. (The name has `D2` = board id 2; the file id is 2.)
* Size by `kioclient5 stat`: 81256 B. This is the same as the serial download, not 82544.
* 3 copies (destination must be an absolute path: a relative path made kioclient5 say "Unknown host" and copied nothing): 0.33, 0.31, 0.41 s. Each: 81256 B, CRC32 `5e8cee05`, md5 `c09119cb...0c57`. The three files and the serial download are byte-equal (`cmp`, md5). The MTP read log says `first=4084`; both ways give the same bytes.
* `ptp_probe`: built in the scratchpad with gcc (host tool, not firmware), run with `-s 5`. `claim interface 2: LIBUSB_ERROR_BUSY` (`kiod6` holds the interface). Not repeated. No stage ran.

### MTP-HW-13.5 Test 5: soak
* 10 more copies, 0.30 to 0.32 s each, all 81256 B, all the same md5. `INFO` after each: `mode=files rec=0 mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0` (10 of 10). Bus still `303a:4041` Device 092.
* LOG after the soak (ring holds the last 48 lines): no E or W line, no `refus` line. Each copy shows `MTP object read begin ... bytes=81256 object_size=81256` and `read complete ... sent=81256/81256 result=0`.
* DIAG after 13 copies and many serial sessions: `heap_free=54752 heap_min=47548`, stack free TinyUSB=5700 mtp_vfs=5916 cmd=5744 main=3376 led=2120. The minimum heap fell from 51576 to 47548 B and two stack margins fell by 0.8 to 0.9 KB while MTP and CDC ran together. The margins stay positive. No reset (no new boot text).

### MTP-HW-13.6 Verdict on MATLAB serial compatibility in files mode
* Works: connect (`MATLAB PING` banner, no stream), `pauseSerial` (`STREAM OFF` gives `STREAM:OFF`), `queryFiles` (`FILES`), `getFile` (`CHUNK`, CRC per chunk, end by n = 0), refused commands in the middle of a download (`DAQS`, `WIFI GET`) do no harm. Serial and MTP give the same bytes.
* The apps still send, and get a refusal for: `DAQS` every 2 s (Calibration App), `WIFI GET` every 5 s (Live Monitor USB source), `STREAM ON` in `resumeSerial`. All three give one `ERR` line, which the apps skip. `CAL`, `WIFI SET` and the other write commands stay refused, so the calibration and WiFi dialogs do not work in files mode (as designed).
* UNVERIFIED: the real MATLAB app (serialport at 115200 with its own DTR handling, `readline` timing, the Java/table CRC) was not run. Only the same command sequence from Python was run.

### MTP-HW-13.7 Problems and notes
1. The `MTP trace` lines at INFO level are back in the LOG ring (MTP-HW-7.1 said they were gone). They fill the 6 KB ring: after 13 copies the boot lines had wrapped out. A flight build should lower the level (see MTP-HW-6.4).
2. `FILES`/`LIST` size (82544) is larger than the readable size (81256). Cosmetic for MATLAB, visible to anyone who compares the two numbers.
3. `CHECK 2 x` is accepted without an error (reads `x` as 0 or 1 file).
4. `kioclient5 copy` needs an absolute destination.
5. 1001 grace window not timed (see 13.1). The bus timeline for the 30 s after the replug was not recorded.

## MTP-HW-14 Crash recovery for panic, abort, interrupt watchdog and task watchdog; MTP trace level fix (2026-11-12 15:05 to 16:10, user at the PC)
Verdict: SAFE on this PC (Linux 7.2, xHCI, kiod6). 8 of 8 crash cycles recovered with no human, no replug and no stale device. The panic types are as good as the task watchdog: the host sees a disconnect at the reset each time. MTP-V-1 did NOT appear. The fix MTP-V-1.3 is not needed on this evidence and is NOT applied. NAND writes: zero. Not sent: `REC ON`, `LEAVE`. No `daq` or `daq_sim` flash, no `usbreset`, no `sudo`.
Board: serial 28:84:85:46:83:04, MODE2 low, no SD card, no 5 V, file id 2 (`NAND_F00002D2.BIN`, 81256 B, md5 c09119cb). Test build: `daq_mtp_force` at commit `a567f8b` (flashed by CDC `DL` and `pio run -t upload`, hash verified, 4041 about 12 s after the flash).
Method: as MTP-HW-8. `lsusb` and `/dev/ttyACM0` every 0.5 s from 5 s before to 45 s after the command. Times are seconds after the command. They have 0.5 s resolution. After each cycle: CDC `INFO`, `LOG` (reset reason, DIAG), one `kioclient5 copy` with md5, and `journalctl -k`.
NOTE: a Claude Code restart cleared the scratchpad. The raw timeline files are lost. The numbers below were copied from the run output before that.

### MTP-HW-14.1 Crash table
| Cycle | Command (what it does) | Reset reason in LOG | Host sees disconnect | 303a:1001 | 303a:4041 + ttyACM | Stale device | INFO after | Copy after |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| 1 | `HANG` (USB task blocked, 8 s task WDT) | 6 (task WDT) | 8.8 | 9.3 | 20.3 | no | clean | md5 c09119cb |
| 2 | `HANG` | 6 | 8.8 | 9.3 | 20.3 | no | clean | md5 c09119cb |
| 3 | `ABORT` (`abort()`) | 4 (panic) | 0.8 | 1.3 | 12.8 | no | clean | md5 c09119cb |
| 4 | `ABORT` | 4 | 0.8 | 1.3 | 12.8 | no | clean | md5 c09119cb |
| 5 | `CRASH` (store to address 0) | 4 (panic) | 0.8 | 1.3 | 12.8 | no | clean | md5 c09119cb |
| 6 | `CRASH` | 4 | 0.8 | 1.3 | 12.8 | no | clean | md5 c09119cb |
| 7 | `INTWDT` (interrupts off in a loop) | 5 (interrupt WDT) | 1.8 | 2.3 | 13.3 | no | clean | md5 c09119cb |
| 8 | `CRASH` during a `kioclient5 copy` loop | 4 | 0.9 (reads fail from 0.7) | 1.4 | 12.9 | no | clean | first good read at 12.5, md5 c09119cb |
* "Clean" INFO = `mode=files rec=0 mounted=1 index=1 blocked_writes=0 nor_blocked=0 lost_pages=0` after every cycle.
* Every cycle: the 1001 window lasts 11 s (the grace countdown), then the host sees a second disconnect and a new 4041. The kernel log shows, per cycle, exactly: `USB disconnect, device number N`, `new full-speed USB device number N+1` (1001), `USB disconnect`, `new full-speed ... N+2` (4041). The device number grows each time, so the host never keeps the old address. No xhci error, no `device not accepting address`, no `cdc_acm` error. Other USB devices stayed on the bus (13 lines in `lsusb`, 12 only in the 0.5 s gaps).
* Time with no MTP: about 20.3 s after a task-WDT hang (8.8 s wait for the WDT + 11.5 s), about 12.8 s after a panic or interrupt WDT (the reset is immediate).
* `ABORT` and `CRASH` both show reset reason 4 (panic): the silent-reboot setting hides the message, so the log cannot tell them apart. The null store is a real fault (it ends in a panic reset, not a hang).
* Cycle 8 (crash in a copy): the running copy failed 0.7 s after the command (when the host saw the disconnect), not after 8 s as with `HANG`. Then 102 copy tries failed fast (0.07 s each) until 4041 came back. 169 of 271 copies in the loop were good (all before the crash and after the recovery: the first good copy was at 12.5 s). Every failed copy leaves no or a partial file: an application must delete it. No bad copy had a wrong md5 (all copies with rc 0 had the right size and md5).
* LOG after each cycle: `reset reason N, USJ pads were clean`, the 10 s countdown, `files mode running (MTP + CDC)`. DIAG: `heap_free=54516 heap_min=51576` (same as before), TinyUSB stack free 5860 to 6628. No sign of memory damage.
* Also seen: a power-on boot (reset reason 1) after the Claude Code restart, 4041 up, same state, copy md5 c09119cb.

### MTP-HW-14.2 Verdict and risk for the DAQ
* A panic, abort, store fault, interrupt watchdog and task watchdog all end in a clean disconnect on this host, a new 1001 window, then 4041 with working MTP and CDC. The way back (`DL`) worked after the cycles, so no BOOT button was needed.
* Why the host sees a disconnect after a panic is not proven. The software reset (and the bootloader) probably drops D+ for a short time on this chip. MTP-V-1.1 feared a stale device: it does not show here, with the HANG result of MTP-HW-8 and 3 more crash types. The IDF panic path, not our code, is the likely reason.
* Not proven: (1) other hosts (Windows, macOS, a USB hub, another xHCI chip). (2) A crash in serial/recording mode (`daq`): `daq_mtp` serial mode has no MTP, and none of the test commands exist there. (3) A crash with a bulk transfer in a partly sent state on other hosts (only kioclient5 was tested). (4) A brown-out or a hard power loss (not a software crash). For a flight, MTP is not used: the board is in serial mode and the host does not need the USB.
* If a stale device ever appears: MTP-V-1.3 (IRAM crash hook) is the candidate. It needs the user's decision and its own hardware proof.

### MTP-HW-14.3 Code changes (commit `a567f8b`, REVIEW (MTP-C), Deviation Log row added)
* `src/usb_files.c`: CDC test commands `ABORT`, `CRASH`, `INTWDT` next to `HANG`, only with `-DUSB_FILES_TEST_CMDS=1` (env `daq_mtp_force`). The strings `HANG`, `ABORT`, `CRASH`, `INTWDT` are in the `daq_mtp_force` image and NOT in the `daq_mtp` image (checked with `strings` on the ELF).
* MTP trace lines at INFO (MTP-HW-13 problem 7), CAUSE FOUND: `src/cmd.c` `do_chunk()` ends with `esp_log_level_set("*", ESP_LOG_INFO)`. A call with "*" removes every per-tag level (IDF `log_level_set`: `esp_log_linked_list_clean()`). So the first serial `CHUNK` (a MATLAB download) removed the 8 WARN levels that `usb_stack_start()` had set, and the trace lines came back. FIX: the tag list is now `usb_files_log_quiet()` (`usb_files.c`, declared in `usb_files.h`). `usb_stack_start()` calls it, and `do_chunk()` calls it after the "*" reset (under `DAQ_MTP_USB`). The `daq` build does not change. The same "*" reset exists in `stream.c`; it runs only when the stream was on, which does not happen in files mode.
* Builds `daq_mtp_force` and `daq_mtp`: no warning. `nm`: 6 `T tud_mtp_*_cb` symbols in `daq_mtp`. Host test `tools/mtp_hosttest/run.sh`: 8427 passed, 0 failed.

### MTP-HW-14.4 Final firmware and Part 2 check on the board
* Flashed: `daq_mtp` (normal build) at code commit `a567f8b`, by CDC `DL` from the force build, `pio run -e daq_mtp -t upload`, hash verified. MODE2 low (FILES). Bus: 1001 first seen 1 s after the flash command, 4041 at about 12 s. INFO clean.
* Part 2 check: 2 serial `CHUNK` commands (`STREAM OFF`, `CHUNK PFS 2 0 16384`, `CHUNK PFS 2 16384 16384`: CRC D090F9F9 and C1E3E811, the same as MTP-HW-13), then 3 `kioclient5` copies (81256 B, md5 c09119cb each). LOG: 29 lines, 0 `MTP trace` lines, boot lines present (`reset reason 11`, `FILES MODE`, `Mount complete`, `NAND mounted read only`, `MTP driver installed`, `files mode running`) and the DIAG line (`heap_free=54512 heap_min=47548`, TinyUSB 5716, mtp_vfs 5916, cmd 6560, main 3488, led 2120). INFO after: clean. A further `CHUNK` after the LOG read worked (CRC 96A23C45 for the first 4096 B).
* The long soak of MTP-HW-13.5 (the ring wrapping after 13 copies) was not repeated; 3 copies gave 0 trace lines. The trace lines are lowered at run time; the first `MTP object read` lines at WARN do not appear.
