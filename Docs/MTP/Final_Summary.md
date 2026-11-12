# MTP Final Summary (for the team lead)

Date: 2026-11-11. Branch: `mtp-usb-mode`. Labels: MTP-F-n. Read this page first (about 2 minutes).

## MTP-F-1 Result

MTP read-only files mode works on Linux with the DAQ. The board shows one storage, "DAQ", with the folders NAND and SD.
A host lists and copies the NAND files. The copies match the source (CRC32 f3d49e4f). We tested with a raw USB client
(probe), libmtp, and KDE kioclient5. Writes, deletes and moves are refused. The host test passes 8427 checks.
The board runs files mode now, with a CDC serial port next to MTP. Not tested: Windows, macOS, SD card, crash recovery (HANG).

## MTP-F-2 Design

* One storage "DAQ" at the root. Folders `NAND` and `SD`.
* NAND file names: `NAND_F<5 digits>D<device ID>.BIN`, for example `NAND_F00002D4.BIN`. The spec said hex. See MTP-F-7.
* SD file names: `uSD_<name>`. No SD card is fitted, so the `SD` folder is empty.
* Read only: the host cannot write, delete, rename or move. The NAND is mounted read only through three layers:
  write-refusing driver wrappers, a read-only LittleFS mount, and no writer task at boot.
* MTP and a CDC serial port run together (one USB device, "IREC DAQ", ID 303A:4041).
  The CDC port accepts only read commands: PING, LED, LIST, FILES, SDLS, CHUNK, CHECK, DUMP, INFO, LOG.
* Mode choice: MODE2 low = files mode in the `daq_mtp` build. On this board, see MTP-F-5.
* Boot: the USB Serial/JTAG port (303a:1001) stays up for a 10 s grace window. Then the device changes to MTP (303a:4041).
  Flashing works during the grace window. A task watchdog (8 s) resets the chip if the USB task hangs.
* No recording in files mode. `REC ON` is refused. Sensors, the chain and WiFi do not start.
* Ways back to a flashable state: `REBOOT` (files mode again, no replug) and `DL` (ROM download mode, flash with no BOOT button).
* `LEAVE` starts serial mode with a writable NAND. Do not use it. It is not tested.

## MTP-F-3 Test table

| Test | Result (from the reports) |
|---|---|
| Read speed, full file | 714 to 718 KB/s in the soak (about 0.7 MB/s). 767 KB/s in the first run. |
| Raw probe, soak (stage 5) | 30 of 30 runs PASS. CRC32 f3d49e4f every time. |
| Raw probe, concurrency | 6 of 6 runs PASS while CDC INFO ran every 200 ms (225 of 225 answered). Total: 36 probe runs. |
| libmtp `mtp-getfile` | 5 of 5 copies match (size and CRC32). Plus one copy in the first run. |
| KDE `kioclient5` copy | 3 of 3 copies match (CRC32), about 0.6 s each. |
| Host test (`tools/mtp_hosttest/run.sh`) | 8427 checks, 0 failed, 15 groups. ASan and UBSan clean. |
| Write refusal | SendObjectInfo and DeleteObject give 0x200E. FormatStore gives 0x2005. KDE write and delete refused. |
| Serial INFO after each test | `blocked_writes=0 nor_blocked=0 lost_pages=0` every time. |
| Power cycle, REBOOT, DL | PASS. DL gives 303a:1001 in about 1 s. Reflash with no BOOT button works. |

## MTP-F-4 NAND writes

* Total NAND writes by the MTP work: 2.8 Mbit (about 173 pages, 354 KB, 0.6 % of the 500 Mbit limit).
* Cause: one 30 s test recording with the `daq_sim` build. The user approved it.
* The NAND now holds one synthetic file: `NAND_F00002D4.BIN` (file ID 2, 323696 bytes). It holds no real data.
* Nothing else wrote to the NAND. The file stays until a format is done.

## MTP-F-5 Firmware on the board, and the builds

Board now: `daq_mtp_force`, the polished build, flashed with DL (Hardware_Report.md MTP-HW-7).
The code change is commit 333c862. The HW-7 report names 83c2640, which is the state commit.
The Deviation Log row for the polish says "not flashed". That row is out of date.

Builds (in this worktree, `pio` from `~/.platformio/penv/bin/pio`):
* `daq`: the serial DAQ. Firmware unchanged. Records at boot if MODE1 is high. Do not flash it on a board with data you want.
* `daq_mtp`: the normal MTP build. Files mode when MODE2 is low.
* `daq_mtp_force`: the test build. Forces files mode with no MODE2 switch. This is the build on the board.

MODE2 conflict: the User Guide (MTP-U-1, MTP-U-8) tells the team to set the MODE2 switch. Merge_Report.md MTP-M-7
and Spike_Report.md (MTP-P-6) say this board has no MODE2 switch wired. GPIO 2 reads high. Check the board before the team uses those steps.

Board history: the original firmware (a PFS-PIL test firmware) was overwritten by the spike on the first flash.
Bootloader, partition table and app were replaced. NAND and pfs_nor were not touched. That firmware is not restored.

## MTP-F-6 Problems found and fixed

* The fork's `tud_mtp_*_cb` callbacks were not linked. Weak defaults answered every command with a failure.
  This is the likely cause of the first "failed to open session". Fixed with a link anchor.
* The host test found 11 handler bugs. Examples: GetObjectHandles root (0xFFFFFFFF) and all (0) were swapped;
  a refused write deadlocked a libmtp-style host. All 11 are fixed.
* USB pad registers: the first pad patch (P3 v1) killed the USB Serial/JTAG pad at 02:03. The board needed BOOT.
  A DL sent to the old spike firmware dropped the board again at 07:50. The current code writes the pad register only when it is dirty.
* Stale device after REBOOT or DL: the host kept a dead device. Fixed with `tud_disconnect()` and a 500 ms delay before restart.
  After the fix, DL gives 303a:1001 within seconds.
* A libmtp USB reset hung the spike firmware and forced a human replug. A watchdog was added to reset the chip with no human.
* KDE `kiod6` (kmtpd) takes the MTP interface. Raw libusb tools get LIBUSB_ERROR_BUSY while KDE runs.
* Static review (MTP-V): TinyUSB task stack 6144 to 8192 B. SD access under one lock. A file-slot reuse bug fixed. DIAG line added to LOG.
  HANG is kept in the test build only.

## MTP-F-7 Still to finish

* HANG (crash recovery, risk MTP-V-1): after a panic or watchdog reset, the host can keep a stale device.
  The board should return by itself after about 10 to 15 s. Not tested. The fix (MTP-V-1.3) is written but NOT applied.
* SD card and Windows: untested (no card fitted). macOS: untested (needs OpenMTP; stop `ptpcamerad` first).
* MATLAB apps in files mode: UNVERIFIED (they expect `DAQBOARD V1` to PING). LED behaviour: from the code only.
* OpenSession refresh (MTP-V-3): on a large file list, the refresh blocks the USB task for up to 10 s (30 s at most).
  EP0 and CDC do not answer then. Low probability. A shorter wait (3 s) is proposed. Not applied.
* NAND file while recording: the User Guide (MTP-U-2) says an open file can be read, with the data written so far. The list refreshes at OpenSession only (MTP-U-11).
* Duplicate fork copy: `mtp_usb/fork` and `tools/mtp_spike/fork` are each 4.9 MB. The spike copy is stale. Decide: delete or keep.
* Merge plan: branch `mtp-usb-mode` is NOT merged. It has 59 commits ahead of `main`. `main` and `nau7802-load-cell` have no MTP code.
  The merge is a team decision. After a merge, `nm -C .pio/build/daq_mtp_force/firmware.elf | grep tud_mtp_` must show `T`.
* Build and flash (no BOOT button, proven):
  ```
  cd /home/atomic/Documents/cpp_projects/IREC_SRAD_DAQ/.claude/worktrees/mtp
  ~/.platformio/penv/bin/pio run -e daq_mtp_force
  python3 tools/mtp_spike/host_cdc.py /dev/ttyACM0 DL 2     # board goes to 303a:1001
  ~/.platformio/penv/bin/pio run -e daq_mtp_force -t upload --upload-port /dev/ttyACM0
  ```
  Then unplug for 3 s, plug in, and wait 10 s for 303a:4041.
* BOOT recovery (board not visible): unplug, hold BOOT, plug in, release BOOT. `lsusb` must show 303a:1001.
  Run the upload command above, unplug for 3 s, plug in without BOOT. The User Guide (MTP-U-9) names `-e daq_mtp`. On this board use `-e daq_mtp_force`.

## MTP-F-8 Where the documents are (Docs/MTP/)

* `Final_Summary.md`: this page.
* `User_Guide.md`: team guide for files mode (steps, Windows, Linux, macOS, limits).
* `STATE.md`: the project log and decisions, in time order.
* `Hardware_Report.md`: board tests MTP-HW-1 to HW-7.
* `HostTest_Report.md`: host test, 8427 checks, the 11 fixes.
* `Merge_Report.md`: how the spike was merged, build wiring, hardware test order, risks.
* `Review_UsbFiles.md`: static review of the USB boot code (MTP-V), open risks.
* `Deviation Log.md`: each change against the old behaviour.
* `Spike_Report.md`: first spike tests, pad issues, watchdog proof.
* `Storage_Design.md`: NAND and SD store design, read-only layers.
* `Boot_Integration.md`: boot paths and build modes.
* `Research.md`: option study (TinyUSB MTP and the design choices).
* `Opus_Hint.md`: static root-cause review, done before the first hardware tests.
* `AGENT_RULES.md`: rules for agents (board safety, NAND limits, Docker).
