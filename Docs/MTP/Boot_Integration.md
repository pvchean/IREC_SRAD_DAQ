# MTP Boot Integration (WIP, paused)

Author: integration agent. Date: 2026-11-11. Branch `mtp-usb-mode`. Language: ASD-STE100. Labels: MTP-B-n.

## MTP-B-1 Status

* DONE and builds: `pio run -e daq` SUCCESS and `pio run -e daq_mtp` SUCCESS (penv pio). No warning in the log (grep for "warning").
* NOT DONE: host test `tools/mtp_store_test.sh` was not run. No hardware test (not flashed, as ordered). The weak-symbol link test (MTP-B-7) was not done.
* NOT DONE: `Docs/MTP/Deviation Log.md` entries and README files-mode lines (see MTP-B-8).

## MTP-B-2 Changed and new code

* `src/console_io.{h,c}` (new): transport. Functions `cio_init`, `cio_set_backend`, `cio_is_serial`, `cio_write`, `cio_read`, `cio_printf`, `cio_flush`.
  The names are `cio_*`, NOT `console_*`: ESP-IDF already defines `console_read` and `console_write` (link error).
* `src/cmd.c`: all `printf` became `cio_printf`, `fflush(stdout)` became `cio_flush()`, the `usb_serial_jtag_*` calls became `cio_write`/`cio_read`.
  `cmd_start()` calls `cio_init()` (the driver install moved there, same values). New: `sd_up()`, `files_mode_cmd()`, REC refusal.
* `src/stream.c`: the write goes through `cio_write(line, len, 20)`.
* `src/io.c/.h`: `io_rec_disable()`, `rec_locked()`, `io_request_serial_once()`; `rec_set(true)` does nothing when locked; RTC_NOINIT flag (two words, magic and complement) read and cleared in `io_init()`; the flag makes `io_usb_files_mode()` false for one boot.
  With `DAQ_MTP`, `io_init()` calls `io_rec_disable()` (after the log line): no auto record, MODE1 ISR removed.
* `src/files_mode.{h,c}` (new): `files_mode_active()`, `files_mode_set_active()`, weak `files_mode_usb_start()` (logs "files mode: no USB stack yet", idles), `files_mode_leave()` (sets flag, `esp_restart()`).
* `src/nor_ro.{h,c}` (new): no-format NOR HAL. LittleFS mounted with `read_only=1` and `format_if_mount_failed=0` (esp_littlefs supports both; PFS is not edited). `write_atomic` refuses and counts. Used by the files-mode mount only. The serial DAQ still uses the stock HAL.
* `src/main.c`: after `io_init()`, `#ifdef DAQ_MTP` and `io_usb_files_mode()` call `files_mode_main()` (never returns). `led_task` shows only the mount error in files mode. `scan_task` and `writer_task` moved above `files_mode_main`.
* `src/CMakeLists.txt`: new sources, `joltwallet__littlefs` in PRIV_REQUIRES. `platformio.ini`: env `daq_mtp` (extends daq, `-DDAQ_MTP=1`).

## MTP-B-3 Behaviour by mode

| Build / MODE2 | Boot | Recording | Console |
|---|---|---|---|
| `daq`, any MODE2 | unchanged | as before | USB Serial/JTAG, same bytes (`cio_printf` = `vprintf`) |
| `daq_mtp`, MODE2 high | serial DAQ as before | LOCKED: `REC ON` gives `REC:ERR recording is disabled in this build (daq_mtp)`; MODE1 high does not record | USB Serial/JTAG |
| `daq_mtp`, MODE2 low | `files_mode_main()` | LOCKED | stub backend (writes dropped, reads wait then give 0) until USB code calls `cio_set_backend()` |

Files-mode boot order: `files_mode_set_active`, `io_rec_disable`, `led_task`, `pfs_early_init`, bus, NAND bind, `nor_ro_littlefs_get`, `mtp_pfs_mount_readonly` (10 tries), `pfs_scan` task, `mtp_store_pfs_init`, `mtp_store_sd_init`, `cmd_start`, `files_mode_usb_start()`, then idle.
NOT started: sensors, chain, `pfs_writer`, stream, `sd_log`, telem/WiFi, `cal_init`, `telem_init`.
Files-mode commands: PING, LED, LIST, FILES, SDLS, CHUNK, CHECK, DUMP, INFO/STATUS (own line with `blocked_writes`), REBOOT (restart, no flush), LEAVE (serial mode once), and since MTP-A `MATLAB PING` and `STREAM OFF` (see MTP-A below). All others: `ERR not available in files mode (read-only)`.

## MTP-B-4 No write path (read from code)

In files mode no code calls `pfs_writer_run`, `pfs_ingest`, `pfs_flush`, page programs, or erases. `pfs_writer_run` is only in `writer_task`, which `files_mode_main` does not create. `pfs_ingest` is only in `sink()`/`emit_cal()` (sensor tasks, not started). `PFSFLUSH`, `FLUSH`, `CAL`, `WIFI`, `TELEM`, `REC ON`, `STREAM` are refused. The mount can still try an erase (Storage_Design MTP-S-2.4): `mtp_pfs_mount_readonly` wraps the NAND HAL and refuses it. In `daq_mtp` serial mode the writer task runs as in `daq`; it has no data (rec locked).

## MTP-B-5 How the USB code hooks in

1. Define `void files_mode_usb_start(void)` (strong). Call `mtp_store_pfs_get()` and `mtp_store_sd_get()` (NULL if not mounted).
2. Start TinyUSB (MTP + CDC). Call `cio_set_backend(&be)` with the CDC read/write; the `cmd` task already runs and then serves the commands.
3. Read the `cio_backend_t` rules in `console_io.h`: `read` must wait for its timeout.
4. The way back: CDC command `LEAVE` (serial mode once), `REBOOT`. A `DL` command (ROM download) is not made.

## MTP-B-6 Open problems

* A weak function in the same component archive may not be replaced by a strong one in another object (linker only pulls members for undefined symbols). Test: add a strong definition in a new .c file and check with `nm` on `firmware.elf`. If it fails, use `WHOLE_ARCHIVE` in `idf_component_register` or define the strong function in `files_mode.c` under `#ifdef DAQ_MTP_USB`.
* Log lines (ESP_LOG) in files mode go to the USB Serial/JTAG console, which TinyUSB owns: they go nowhere.
* No hardware test of `nor_ro` (a read-only LittleFS mount of the real store) or of the files-mode boot.

## MTP-B-7 Next steps, in order

1. Run `tools/mtp_store_test.sh`. 2. Do the weak-link test above. 3. Write the Deviation Log entries (MODE2 "files = not used yet" in `board.h` and README now used; `daq_mtp` locks REC; `cio_*` names; `cmd_start` installs the driver through `cio_init`). 4. Edit the README files-mode lines (line 26, `board.h` line 33 comment). 5. Flash only after the spike agent agrees; check `INFO` shows MODE1 low or use `daq_mtp`.

## Link check

Date: 2026-11-11. Build: `pio run -e daq_mtp` passed (RAM 51.9 %, flash 25.4 %).
`files_mode_usb_start` is in `firmware.elf` (`nm`, xtensa-esp32-elf-nm). Address 0x4200943c.
The symbol type is `W` (weak). The weak default is linked.
The weak-link test (MTP-B-6) is not done. This check only shows that the symbol exists.
No board was flashed.

## MTP-A MATLAB connect and file download in files mode (REVIEW (MTP-A), 2026-11-12)

Problem (Hardware_Report.md MTP-HW-10.3): `MATLAB PING` was refused in files mode. The MATLAB apps send `MATLAB PING` and wait for the banner. They could not find the board.

### MTP-A-1 What the apps send (read from `src/*.m`, not run)
| Step | Command | Needed in files mode? |
|---|---|---|
| Connect (Calibration App, Live Monitor, Validation Viewer): scan, open port at 115200, wait 1.5 s, look for the banner | `MATLAB PING` | YES. Now allowed. |
| Calibration App, every 2 s after connect | `DAQS` | No. The reply only fills a label. A refusal line is ignored by the app. |
| Live Monitor USB source, every 5 s | `WIFI GET` | No. The reply only shows WiFi state. Refused: the app ignores it. |
| Files & Plot, before a file list or a download (`pauseSerial`) | `STREAM OFF`, then waits up to 5 s for `STREAM:OFF` | YES. Without the reply the app stops with a timeout error. Now allowed. |
| File list | `FILES` (reply `FILES:BEGIN`, `FILE ...`, `FILES:END`) | Already allowed. |
| Download, chunk by chunk | `CHUNK <src> <name> <offset> <len>` | Already allowed. |
| After the list or download (`resumeSerial`) | `STREAM ON`, no reply wanted | No. Stays refused. The app ignores the refusal line. |
| Calibration App, calibration buttons | `CAL GET`, `CAL ...`, `WIFI ...`, `REBOOT` | No. They write settings. Stay refused (REBOOT is the files-mode restart). |

The apps skip any line that does not match what they wait for (`waitLine`), so the `ERR not available...` lines do no harm.

### MTP-A-2 The change
* New `src/files_mode_cmd.h`: `files_mode_classify(tok, arg)` is the allowlist as a pure function (no I/O, no state). `files_mode_cmd()` in `src/cmd.c` calls it and does the action.
* `MATLAB PING` (exactly these two words): prints `01010101 10101010 DAQBOARD V1 01010101 10101010`. It does NOT call `stream_enable()`. No task starts. No state changes. In serial mode the same command also starts the stream: that code is unchanged.
* `STREAM OFF`: prints `STREAM:OFF`. It does not call `stream_enable()` (no stream exists in files mode). `STREAM ON` and `STREAM` with no word: refused.
* The second word is read with `strtok` only for `MATLAB` and `STREAM`. The other commands read their own arguments, so they are not changed.
* Not allowed, on purpose: `DAQS`, `WIFI`, `TELEM`, `CAL`, `REC`, `FLUSH`, `PFSFLUSH`, `COMPARE`, `PROFILE`, `STREAM ON`. `DAQS` and `WIFI GET` look read-only, but they read state of tasks that do not run in files mode, and no app step needs them.
* Commands now allowed in files mode: `PING`, `MATLAB PING`, `STREAM OFF`, `LED`, `LIST`, `FILES`, `SDLS`, `CHUNK`, `CHECK`, `DUMP`, `INFO`, `STATUS`, `REBOOT`, `DL` (`daq_mtp` builds), `LEAVE`, and (in `usb_files.c`) `LOG`.

### MTP-A-3 Test
* `tools/mtp_hosttest/cmdtest.c` (run by `tools/mtp_hosttest/run.sh`) compiles the real header. 80 checks: each allowed command, `MATLAB` and `STREAM` with a wrong or no second word, and the commands that write (`REC`, `FORMAT`, `CAL`, `WIFI`, ...) stay refused.
* Builds `daq_mtp`, `daq_mtp_force`, `daq`: no new warnings.
* NOT tested on the board and NOT tested with the real MATLAB app. UNVERIFIED. Check list for later: open the port with DTR set, send `MATLAB PING` (expect the banner, no number lines after it), `STREAM OFF` (expect `STREAM:OFF`), `FILES`.
* Note: with no DTR the CDC gives no reply (MTP-HW-10.3). MATLAB `serialport` sets DTR by default, so this is expected to work. UNVERIFIED.
