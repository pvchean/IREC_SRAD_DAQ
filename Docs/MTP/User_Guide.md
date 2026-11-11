# DAQ files mode: user guide for the team

For: team members who plug the DAQ into a laptop to get the recorded files.
Firmware: build `daq_mtp` (files mode). The board today runs `daq_mtp_force`, an older test build. The changes of commit 333c862 (more stack, quiet log, no `HANG`) are NOT flashed.
Status words: TESTED = done on the real board with Linux. UNTESTED = never tried on this hardware.

## MTP-U-1 Enter files mode
1. Set the MODE2 switch low.
2. Remove the power. Wait 3 seconds. Connect the power again (power cycle). A reset button is not enough.
3. Wait 10 seconds. The board shows a USB serial port (303a:1001) for 10 seconds first. This is the flash window. The files do not show yet.
4. After the wait, the board shows as the USB device "IREC DAQ".
5. LEDs in files mode (from the code, not checked on the bench): Working blinks. NAND is on when the NAND mounted. ERROR is on when the NAND did not mount. Recording, Auto, WiFi and SD LEDs stay off.
6. In files mode the board does not record, does not read sensors and does not use WiFi. It only gives files.

## MTP-U-2 What you see
* Device: "IREC DAQ". One storage: "DAQ". Two folders: `NAND` and `SD`.
* NAND files: `NAND_F<5 digits>D<device ID>.BIN`. Example: `NAND_F00002D4.BIN` is file 2 of device 4.
* SD files: `uSD_<name>`. The SD card is not installed yet. The `SD` folder is empty.
* Everything is read-only. You cannot delete, rename, move or write. The host gets an error (this is correct).
* A file that is still open for recording can be read. It has the data written so far.

## MTP-U-3 Windows (UNTESTED)
1. Open File Explorer. Look under "This PC" for "IREC DAQ".
2. Open it, then "DAQ", then `NAND`.
3. Copy the `.BIN` files to your laptop with copy and paste.
4. If Windows asks for a driver: it should not. Report it.
5. Do not use "Move" or "Delete". The board refuses them.

## MTP-U-4 Linux
Desktop file manager (gvfs, Dolphin): UNTESTED. It needs the packages `gvfs-mtp` (GNOME) or `kio-extras` (KDE). On the test PC the file manager did not list the board (MTP-HW-6.1). The cause is on that PC, not in the board. Replug once and try again.
Command line with libmtp (TESTED):
1. `mtp-files` shows the file list with the numbers (IDs).
2. `mtp-getfile <ID> <name>` copies one file. Example: `mtp-getfile 3 NAND_F00002D4.BIN`.
3. `mtp-folders` shows the folders.
4. Close any other program that uses the device first (a file manager can block it).

## MTP-U-5 macOS (UNTESTED)
1. macOS has no built-in MTP. Install OpenMTP (free).
2. The macOS process `ptpcamerad` takes the device first. Stop it before you open OpenMTP. Try: `killall ptpcamerad`. It starts again by itself.
3. Copy the `.BIN` files with OpenMTP.
4. We did not test this. Ask the software team if it fails.

## MTP-U-6 Decode a .BIN file
A `.BIN` file holds raw records (PT and load cell). It is not a text file.
1. CSV: in the main repository, run `python3 tools/bin_to_csv.py FILE.BIN [-o OUTDIR]`. You get one CSV with time, raw count, volts, and PSI or N (with the calibration of the file).
2. MATLAB `DAQ_Calibration_App`, window "Files & Plot...": it lists `.BIN` files in its download folder (`<output folder>/Downloads`, you can change it). Put the copied files there. Select a file to plot or export CSV. This function is in the app code; we did not run it with an MTP copy.
3. MATLAB `DAQ_Validation_Viewer`: it accepts a `.BIN` file in its file dialog. It reads the load cell records only.
4. The records have no header. Keep the file name: it holds the file ID and the device ID.

## MTP-U-7 The serial port in files mode
The board also shows a serial port (CDC), for example `/dev/ttyACM0` or `COMx`, while MTP runs. Use it with any terminal. End each command with Enter.
* Commands that give information: `INFO` (state), `LOG` (last log lines, ends with a `DIAG` line, then `END`).
* Commands that change the mode: `REBOOT` (restart, back to files mode, no replug), `DL` (download mode, to flash new firmware). Do NOT use `LEAVE`: it starts serial mode with a read-write NAND.
* Other allowed commands: `PING`, `LED`, `LIST`, `FILES`, `SDLS`, `CHUNK`, `CHECK`, `DUMP`. Everything else gives `ERR not available in files mode (read-only)`.
* `REC ON` and `FORMAT` do not work. This is on purpose.
* MATLAB apps: `DAQ_Calibration_App` and `DAQ_Live_Monitor` scan all ports and send `MATLAB PING`. They expect the answer `DAQBOARD V1`. In files mode the allowlist has `PING`, but we did not check that the answer has this text. UNVERIFIED. Live data does not exist in files mode. Use the apps in normal serial mode.

## MTP-U-8 Back to normal serial mode
1. Set the MODE2 switch high.
2. Do a power cycle (remove power, wait 3 seconds, connect).
3. The board is a normal serial DAQ again (USB Serial/JTAG, 303a:1001).
4. Before a recording: make sure MODE1 is as you want. With MODE1 high the `daq` build records at boot.

## MTP-U-9 The PC does not see the device
1. Wait the full 10 seconds after power-up. Do not judge earlier.
2. Check that MODE2 is low. Do a full power cycle. Try another cable (data cable, not charge only) and another port.
3. Run `lsusb` (Linux). `303a:4041` is files mode. `303a:1001` is the flash window or normal mode.
4. Close other programs that use the device. Replug.
5. Send `REBOOT` on the serial port, if the port exists.
6. If the port shows 303a:1001 and never changes to 4041: flash again with `DL` or the BOOT recovery below.
7. BOOT recovery: unplug. Hold the BOOT button. Plug in. Release BOOT. `lsusb` shows 303a:1001. Run `~/.platformio/penv/bin/pio run -e daq_mtp -t upload --upload-port /dev/ttyACM0`. Unplug 3 seconds. Plug in without BOOT.
8. Warning: never flash `-e daq` on a board that holds data you want to keep, if MODE1 is high.

## MTP-U-10 SD card
The micro SD card is not installed yet. The `SD` folder is empty. This is correct. When a card is fitted, its files show as `uSD_<name>`. This path is UNTESTED on the real board (only code review and host tests).

## MTP-U-11 Limits
* Read speed is about 0.7 MB/s (714 to 767 KB/s measured). A 100 MB file takes about 2.5 minutes. Do not unplug during the copy.
* A copy with `mtp-getfile` has a start delay of about 0.5 s (the file list refresh).
* The host reads the file size when it opens the session. If the size changes (file still recording), copy again.
* The NAND file list is refreshed when a host opens a session. Replug to see new files.
* After a crash in files mode, a stale USB device can stay for about 10 s (risk MTP-V-1). Wait, then replug.

## MTP-U-12 Known limits and untested
| Item | Status |
| :-- | :-- |
| Linux libmtp: list, copy, random read, CRC | TESTED (30 of 30 runs, 5 of 5 copies) |
| Serial `INFO` during an MTP copy | TESTED (225 of 225 answered) |
| Write, delete, format by a host | Refused, TESTED |
| Windows File Explorer | UNTESTED |
| Linux file manager (gvfs, Dolphin) | UNTESTED (not testable on the test PC) |
| macOS with OpenMTP, `ptpcamerad` | UNTESTED |
| Copy of a file larger than 1 MB, or a 100 MB file | UNTESTED (speed is an estimate) |
| SD card files | UNTESTED, no card |
| MATLAB app finds the board in files mode | UNVERIFIED |
| `LEAVE` | Never tested. Do not use. |
| Crash recovery (watchdog reset in files mode) | UNTESTED on hardware |
| LED indications in files mode | From the code, not checked |
| Commit 333c862 changes (stack, log, no `HANG`) | NOT FLASHED, builds only |
