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
Files-mode commands: PING, LED, LIST, FILES, SDLS, CHUNK, CHECK, DUMP, INFO/STATUS (own line with `blocked_writes`), REBOOT (restart, no flush), LEAVE (serial mode once). All others: `ERR not available in files mode (read-only)`.

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
