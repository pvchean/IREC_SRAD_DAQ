# MTP Storage Design (read-only storage side of the files mode)

Author: storage analysis agent. Date: 2026-11-11. Branch: `mtp-usb-mode`.
Scope: how the firmware boots, how PFS (NAND) and the SD card are read, and the storage layer that the MTP code will use.
This document has no USB design. Language: ASD-STE100 Simplified Technical English. Section labels are MTP-S-n.
Status of the code: `src/mtp_store*.{h,c}` compile in `pio run -e daq` with no warning. A host test passes. Nothing was flashed. The NAND was not touched.

## MTP-S-0 Summary

* PFS has **no read-only mount**. `pfs_mount()` can write to the NAND in three cases (MTP-S-2.4). The fix is a **HAL shim** in the DAQ code:
  `mtp_pfs_mount_readonly()` gives PFS a NAND HAL that refuses `write_page`, `erase_block`, and `mark_bad_block`, and a NOR HAL that refuses `write_atomic`.
  PFS is not edited. A NAND write is then impossible, and a counter shows each refused call.
* The files mode must not run the sensors, the PFS writer, the chain, the stream, the SD logger, WiFi, or the serial command task (MTP-S-6).
  The PFS scan task must run: it only reads.
* One PFS file = one MTP file, in the folder `NAND`. One SD file = one MTP file, in the folder `SD`. Names follow `tools/daq_get.py` (MTP-S-4).
* The PFS build in use is **v0.3.1** (managed component), not the repository head (0.4.0). The mount code is the same for this analysis. The status struct is different (handled in the code).
* The build must use `~/.platformio/penv/bin/pio`. The system `/usr/bin/pio` fails at the bootloader image step (`No module named 'intelhex'`). See MTP-S-8.

## MTP-S-1 Firmware today

### MTP-S-1.1 Boot order (`src/main.c`, `app_main()`)

1. `led_sr_init()`, then `io_init()`: pull-ups on MODE1, MODE2, ADDR0..2, 50 ms settle, read the pins, install the GPIO ISR service and the MODE1 button ISR.
2. `cal_init()` (NVS read), `telem_init()` (NVS read, WiFi mode; no radio yet), `led_task` (core 0).
3. `chain_start()`: UART1 and UART2 tasks (`ch_tx`, `ch_rx`, `ch_hello`).
4. `pfs_early_init()`, bind the SPI bus (80 MHz), bind the ZD35Q1GC NAND driver, get the NOR HAL (LittleFS in partition `pfs_nor`, mounted at `/pfs`).
5. Start the sensors (`ads1115_start`, `nau7802_start`): the data goes through `sink()`; `pfs_ingest()` buffers it in RAM during the mount.
6. `pfs_mount()` (retry every 1 s), then `pfs_writer` (core 1, priority 5) and `pfs_scan` (core 1, priority 2, ends when the file index is ready).
7. `stream_start()`, `sd_log_start()` (SD task), `cmd_start()` (USB Serial/JTAG driver and the `cmd` task), `telem_start()` (WiFi).
8. The main loop prints a status line every 10 s.

### MTP-S-1.2 Where MODE2 is read

`src/io.c`, `io_init()`: `s_files = gpio_get_level(MODE2_PIN) == 0;` (GPIO 2, pull-up, so a floating pin is serial mode).
It is read **once**, after the 50 ms settle. `io_usb_files_mode()` returns it. Nothing else uses it today.
`s_auto = MODE1 high` and `s_rec = s_auto`: with MODE1 high, **recording starts at power-up**. The MODE1 ISR toggles `s_rec` when `s_auto` is true.
So in files mode the code must force `rec` off and stop the ISR (MTP-S-7).

### MTP-S-1.3 Serial protocol (`src/cmd.c`)

Text lines on USB Serial/JTAG. `cmd_task` reads one byte at a time. A line ends at `\n`, `\r`, or 500 ms of idle. `run()` splits it with `strtok`.

| Command | Action | Writes? |
|---|---|---|
| `PING`, `MATLAB PING`, `STREAM ON/OFF`, `LED n` | banner, live readings, LED brightness | no |
| `STATUS`, `INFO` | one status line (`rec`, `mounted`, `file`, `sd`, ...) | no |
| `LIST` | `FILE id= flags= pages= first_offset= size=` per PFS file | no |
| `FILES` | `FILE PFS <id> <bytes> <flags> <start>` and `FILE SD <name> <bytes>` | no |
| `SDLS` | SD directory | no |
| `CHUNK PFS <id> <off> <len>` / `CHUNK SD <name> <off> <len>` | reply `CHUNK:<off> <n> <crc32>` then n raw bytes (max 16384) | no (SD: calls `sd_log_flush`) |
| `CHECK [id [max]]`, `DUMP id off len`, `COMPARE n` | decode and test, hex dump | no |
| `DAQS` | boards in the chain | no |
| `CAL ...`, `WIFI ...`, `TELEM ...` | calibration and WiFi settings | NVS only |
| `REC ON/OFF` | sets `rec` | **starts NAND writes** |
| `PFSFLUSH`, `FLUSH`, `REBOOT` | flush the PFS writer / SD, reset | `pfs_flush` waits for the writer |

`CHUNK PFS` uses `pfs_read()` with the file offset as given by `FILES` (`start` = first whole 8-byte record).
Output uses `printf` (VFS on the USB Serial/JTAG driver) and `usb_serial_jtag_write_bytes()` in 1 KB pieces. These are not usable with TinyUSB.

### MTP-S-1.4 Tasks that run in serial mode

| Task | Core / prio | Started by | Needed in files mode |
|---|---|---|---|
| `led` | 0 / 1 | main | yes (LEDs); its error rule needs a change (MTP-S-7) |
| `ads`, `nau` (+ ISRs on GPIO 17, 16) | 1 / 6, 0 / 6 | `ads1115_start`, `nau7802_start` | **no** |
| `ch_tx`, `ch_rx`, `ch_hello` | 0 | `chain_start` | **no** |
| `pfs_writer` | 1 / 5 | main | **no (the only task that writes to the NAND)** |
| `pfs_scan` | 1 / 2 | main | **yes (read only; builds the file index)** |
| `stream` | 0 / 1 | `stream_start` | no |
| `sd` | 0 / 2 | `sd_log_start` | **no** (it mounts the card and writes) |
| `cmd` | 0 / 1 | `cmd_start` | no (needs the USB Serial/JTAG driver) |
| `telem` + WiFi | 0 / 2 | `telem_start` | no |
| `toggle`, `prof` | test builds only | | no |

### MTP-S-1.5 How the two storages are mounted

* **NAND (PFS)**: `pfs_mount(ctx, nand, nor)`. Path A loads the block tables from the NOR store (LittleFS on the internal flash) and checks them with a frontier sweep (about 1 page read per block). Path B scans the NAND. Mount time 1.19 s at 80 MHz (README). The file index is ready after the scan task ends, about 3.6 s after boot (README).
* **SD**: `esp_vfs_fat_sdmmc_mount("/sdcard", ...)` in `sd_log.c`, SDMMC slot 1, 1-bit (CLK 21, CMD 47, D0 14), `format_if_mount_failed = false`. The SD task retries every 2 s when there is no card.

## MTP-S-2 PFS read-only facts

### MTP-S-2.1 API for reading (`pfs.h`, v0.3.1 and 0.4.0 are the same here)

| Need | Call | NAND read? | Remark |
|---|---|---|---|
| Number of files | `uint16_t pfs_file_count(ctx)` | no | closed files in the index (+ the active file if it has pages). Max 128 (`PFS_MAX_TRACKED_FILES`); the oldest entry is dropped beyond that. |
| List (id, flags, pages, first byte, approximate size) | `pfs_file_info(ctx, index, &fi)` | no | index 0 = oldest. `size_bytes = (last_seq+1) * 1876`: a **multiple of the page payload**, not exact. |
| Exact size | `pfs_file_stat(ctx, id, &fi)` | yes (last page, and a short probe past the end) | sets `PFS_FILE_SIZE_EXACT`. Returns `PFS_ERR_CORRUPT` if the last page cannot be read. |
| "Open" | none | | PFS has no open and no handle. A file is its `uint16_t file_id`. |
| Random or sequential read | `pfs_read(ctx, id, offset, dst, len, &out)` | yes | offset counts the stored bytes of the file. It reads page by page and may return less than asked at a page end or the file end. `PFS_ERR_TRIMMED` below `first_offset`; `PFS_ERR_EOF` at the end. |
| Status | `pfs_get_status(ctx, &st)` | no | `index_ready`, `mounted`, `active_file_id`. v0.3.1 has no `struct_size` field; 0.4.0 needs it set. The backend handles both. |

No call gives the device ID, a time, or a name of a file. `pfs_file_info_t` is `{file_id, flags, pages, first_offset, size_bytes}`.
The device ID is in each page header (`device_id` of `pfs_config_t`) but no public call gives it back. The time is the sensor time in the records (microseconds since boot), not a clock.

### MTP-S-2.2 File layout

* One PFS page is 2048 bytes of NAND (`NAND_PAGE_SIZE`). 9 Reed-Solomon codewords (211 data + 16 parity bytes), 144 parity bytes in all.
  Protected area 1899 bytes, minus 23 bytes (13 header, 6 footer, 4 CRC) = **1876 bytes of payload per page** (`PFS_PAYLOAD_SIZE`). The code has a `_Static_assert` for this value.
* A block has 64 pages; the drive has 1024 blocks (128 MiB raw, 117 MiB payload). It is a ring: new data overwrites the oldest block.
* A file is a series of pages with `seq_num` 0, 1, 2, ...; byte offset `o` is in page `o / 1876` at position `o % 1876`. The last page can be short (`payload_len`).
* A file that the ring overwrote at its start is *trimmed* (`first_offset > 0`). The data stream is continuous across pages, so a record can cross a page end. 1876 is not a multiple of 8, so the first whole record of a trimmed file is at `start = round_up(first_offset, 8)` (up to 4 bytes after the page start). `FILES` does the same. **The MTP file starts at `start`, so it is a clean series of 8-byte records.** File size in MTP = PFS size - start.

### MTP-S-2.3 Cost of a read

* A random read of any offset costs **one page read** when the file has no hole: `pfs_read_seq()` computes the page address from the anchor (`first_vlpn`, `first_seq`) and reads it. Holes (a lost or skipped page) cost a few more reads (limits: `PFS_MAX_READ_CORRECTIONS`, `PFS_MAX_ALLOC_ATTEMPTS` = 8).
* There is **no page cache** in PFS: each `pfs_read()` call reads the page again. Reading 512 bytes at a time would read each page about 4 times. The backend keeps a one-page cache (1876 bytes) and always reads whole pages at page-aligned offsets. The host test confirms 1 NAND read for each page in a sequential read.
* Time per page: **UNVERIFIED** (not measured). Estimate: 2112 bytes at 80 MHz single wire = 0.21 ms, plus the chip read time and the CRC and RS check in the CPU, so about 0.3 to 1 ms. The Path A mount reads about 1024 pages in 1.19 s with the NOR work and CPU work included, so 1 ms per page is a safe upper bound. That is at least 1.8 MB/s, which is more than a full-speed USB link gives (about 1 MB/s). A page that needs an RS repair or a second read costs more.
* `pfs_read()` takes the `reader_lock` and the `io_lock`. It is safe to call from one task while the scan task runs, and while the serial `CHUNK` command runs.

### MTP-S-2.4 Does mounting write to the NAND? (critical)

**Yes, in some cases. PFS has no read-only flag.** `pfs_mount()` (v0.3.1 `pfs.c` lines 2546 to 2640; the same steps in 0.4.0) calls these write paths:

| # | Where | When | What it writes |
|---|---|---|---|
| 1 | Path A, `pfs_frontier_check()` -> `pfs_block_cleanup()` | a free block still holds old valid data (DEV-144, after an interrupted migration or erase) | `erase_block`; on an erase failure `mark_bad_block` |
| 2 | Path B, `pfs_mount_scan()` duplicate rule -> `pfs_block_cleanup()` | two blocks have the same VLBN (after an interrupted migration) | `erase_block` of the loser |
| 3 | `pfs_hydrate()` (write-head recovery, every mount) | the head block has no valid page but is not clean (an interrupted erase or a torn page 0) | `erase_block` of the head block |
| 4 | Path B only, `pfs_nor_commit()` | Path B mount | NOR store write (internal flash, not the NAND) |

In a normal case after a clean power-off, none of 1 to 3 happens. After a power cut during a recording, one of them can happen. The data in these blocks is not needed (free or empty blocks), but the user wants **zero** NAND writes.
Also: `nand->init()` (called by `pfs_mount()`) sends RESET and SET FEATURE (ECC on, block-protect bits cleared). These are chip register writes, not array writes. Whether the protect bits stay after power-off is **UNVERIFIED**; the data sheet is in `Docs/Hardware Docs` of the PFS repository (not read).
Nothing else writes: `pfs_read`, `pfs_file_*`, and `pfs_scan_step` only read (checked: the only `write_page` calls are in the writer step and in the migration, which only the writer calls; `erase_block` and `mark_bad_block` are reached only from the paths in the table and from the writer).
NOR side: `pfs_nor_littlefs_get()` mounts LittleFS with `format_if_mount_failed = 1`. A damaged NOR store would be **formatted** (NOR flash, not NAND).

### MTP-S-2.5 How the read-only mount works (what was added)

`mtp_pfs_mount_readonly(ctx, nand, nor)` in `src/mtp_store_pfs.c` calls `pfs_mount()` with two wrapper HALs:

* NAND wrapper: `init`, `read_page`, `is_bad_block` go to the real driver. `write_page`, `erase_block`, `mark_bad_block` return `NAND_ERR_PROTECTED` and add 1 to `mtp_pfs_blocked_writes()`. The real driver never gets the call.
* NOR wrapper: `mount` and `read` go to the real HAL. `write_atomic` returns `PFS_HAL_ERR_IO` and adds 1 to `mtp_pfs_blocked_nor_writes()`.
* PFS then treats the refused call as a failed erase: it lowers the health of that block **in RAM only** and goes on (`pfs_block_fail_erase`, `pfs_block_retire` only touch free blocks with no data). The mount still gives the file index and reads of all data blocks. The status shows `degraded` or `nor_failed`; nobody uses it in files mode.
* The call returns `PFS_ERR_INVALID_ARG` in serial mode.
* **Rule for the caller:** do not start `pfs_writer_run()`, do not call `pfs_ingest()` or `pfs_flush()`, and do not call `pfs_mount()` directly. Without the writer task there is no other path to `write_page`.
* After the mount, main should log `mtp_pfs_blocked_writes()`. A value above 0 means that a recording was cut by a power loss (case 1 to 3). It is information, not an error. The NAND is unchanged.
* This needs no change in the PFS repository. If a flag in PFS is wanted later, it is a change to the PFS spec (not made).
* **UNVERIFIED on hardware:** the shim was tested on the host with a fake PFS (it checks that the real driver is never called). A real mount on the board with the shim was not run (the rules allow reads only, and the build was not flashed).

### MTP-S-2.6 A file that is open for recording

In files mode nothing records, so no file is active. At the mount `file_has_pages` is false, so every file is a closed file in the index, and `pfs_file_count()` does not count the new `active_file_id` (the ID that the next recording would use).
A file that was cut by a power loss ends at its last good page; `pfs_file_stat()` gives its exact size from that page. The bytes that were still in the RAM buffer at the cut are lost; that is not a storage issue.
MODE1 high at boot (auto record) must not start a recording in files mode (MTP-S-7).

## MTP-S-3 Storage API (`src/mtp_store.h`)

| Member | Type | Meaning |
|---|---|---|
| `id` | `uint32_t` | 1 = NAND, 2 = SD. The MTP layer builds the 32-bit storage ID from it. |
| `name` | `const char *` | "NAND" or "SD": the folder name / storage description |
| `present()` | `bool (*)(void)` | readable now. NAND: initialised. SD: card in and mounted (CMD13 test; a mount try at most every 2 s) |
| `refresh()` | `int (*)(void)` | rebuild the file list (call at session open); returns the count or -1 |
| `count()` | `uint32_t (*)(void)` | files in the list |
| `info(i, &out)` | `bool (*)(uint32_t, mtp_file_info_t *)` | `{name[40], size, mtime, size_exact}`; false if `i` is out of range |
| `read(i, off, buf, len)` | `int32_t (*)(uint32_t, uint64_t, uint8_t *, size_t)` | random read; bytes read (short only at the end); 0 = end; < 0 = hard error |
| `capacity()` | `uint64_t (*)(void)` | total bytes (MTP StorageInfo). Free space is always 0. |

Entry points: `mtp_store_pfs_init(ctx)`, `mtp_store_sd_init()`, `mtp_store_pfs_get()`, `mtp_store_sd_get()` (NULL until initialised, and always NULL in serial mode).
Extras: `mtp_pfs_mount_readonly()`, `mtp_pfs_blocked_writes()`, `mtp_pfs_blocked_nor_writes()`, `mtp_store_pfs_lost_pages()`.
Rules: one caller at a time; the list is fixed between two `refresh()` calls (so object handles stay valid). `mtime` is 0 for NAND files (PFS has no clock; use a fixed date in the MTP layer) and the FAT time for SD files.
**Read errors:** a page that PFS cannot read (`PFS_ERR_CORRUPT`) is returned as 1876 bytes of `0xFF` and counted. The offsets stay true, the transfer does not stop, and a decoder reads `0xFF` as an unknown META record (ignored). The count is `mtp_store_pfs_lost_pages()`; the MTP layer or the log should show it. If the size is only approximate (`size_exact` false, because the last page was unreadable), the part after the real end is also `0xFF`.

## MTP-S-4 File names

Folder `NAND`: `NAND_F<file id, 5 digits>D<device id>.BIN`, for example `NAND_F00012D3.BIN`.
Folder `SD`: `uSD_<name on the card>`, for example `uSD_F00012D3.BIN` (the logger writes `F<PFS file id, 5 digits>D<device id 0..7>.BIN`, `sd_log.c`).

* These are the local names that `tools/daq_get.py` gives to a download (`NAND_F%05dD%d.BIN`, `uSD_<name>`). A file that a user copies out over MTP has the same name as a download over serial, so `file_compare.py` and `compare_sd.py` work on both. The prefix also stops a name clash when the user copies both folders into one folder.
* The device ID is the **current ADDR pins** (`io_addr()`), as in `daq_get.py`. PFS gives no per-file device ID. If the user changed the pins between the recording and the copy, the name shows the new ID. This is UNVERIFIED as a field problem; the file content is not affected.
* There is no time in the name, because no source has a clock. The file ID counts the boots (it is the PFS `boot_count`) and rises with each recording.
* A PFS file of a remote board does not exist; remote data is on the SD card (`...D<remote id>`).
* Alternative if the user prefers short names: `F00012D3.BIN` in both folders. Change the `snprintf` in the two `info` functions.

## MTP-S-5 SD backend (`src/mtp_store_sd.c`)

* Mount: its own `esp_vfs_fat_sdmmc_mount("/sdcard")` with the same pins as `sd_log.c`; `format_if_mount_failed = false`; `max_files = 2`. `sd_log_start()` must not run in files mode (it would also mount `/sdcard` and open files for append).
* Read-only: ESP-IDF 6.1 VFS-FAT has **no read-only mount flag** (`esp_vfs_fat_mount_config_t` has none; `FF_FS_READONLY` is 0). The backend never calls anything that writes: files open with `fopen(path, "rb")` (= `f_open` with `FA_READ`), never formats, never creates. FatFS does not write at mount or at a read-only close. (UNVERIFIED on a card: no card is installed.)
* No card: `present()` returns false, `refresh()` returns -1, `read()` returns -1. A failed `fopen` or a failed CMD13 drops the mount, and the next `present()` mounts again (at most every 2 s).
* List: regular files in the root only (folders such as `System Volume Information` are skipped), up to 256 files, names of 15 characters or less. `mtime` is the FAT time.
* One file is open at a time (the last one read); a read of another file closes it.
* Speed: default SDMMC clock (20 MHz, 1-bit, about 2 MB/s at best). A faster clock is possible (`SDMMC_FREQ_HIGHSPEED`) but not tested. USB full speed is the limit anyway.

## MTP-S-6 What must not run in files mode

Goal: the board is a read-only drive. Rules:

1. No recording: `rec` is false, the MODE1 ISR is off, `pfs_writer` does not start, `sink()` never runs, `pfs_ingest()` is never called.
2. No sensor tasks, no sensor ISRs (GPIO 17 and 16), no I2C use.
3. No chain (UART1 and UART2): it forwards packets and writes remote data to the SD card.
4. No `stream`, no `sd_log` (SD task), no `telem` (WiFi), no `cmd_start()` (USB Serial/JTAG driver install: the PHY belongs to TinyUSB).
5. No `cal_init()`/`telem_init()` (they read NVS and `telem_init` allocates PSRAM): not needed. `cal` and `telem` commands write NVS only; if a CDC command task is kept, refuse them.
6. Run: `led_sr_init`, `io_init`, `led_task`, `pfs_early_init`, bus and NAND bind, `mtp_pfs_mount_readonly`, `pfs_scan` task, `mtp_store_pfs_init`, `mtp_store_sd_init`, then the USB code (another agent).

### How to do it safely: the boot path change in `main.c`

Branch right after `io_init()`:

```c
io_init();
if (io_usb_files_mode()) { files_mode_main(); /* never returns */ }
```

`files_mode_main()` (new, in main.c or `files_mode.c`) does:
1. `io_rec_disable()` (new in `io.c`): `gpio_isr_handler_remove(MODE1_PIN); s_auto = false; s_rec = false;`
2. `xTaskCreatePinnedToCore(led_task, ...)` (see MTP-S-7 for the LED rule).
3. `pfs_early_init`, `spi_bus_esp32s3_get`, `nand_zd35q1gc_bind`, `pfs_nor_littlefs_get` (same code as now).
4. `while (mtp_pfs_mount_readonly(s_pfs, nand, nor) != PFS_OK) { delay 1 s; }`, with a limit of tries, then show the error on the LEDs.
5. Create the `pfs_scan` task (same static task as now). **Do not create `pfs_writer`.**
6. `mtp_store_pfs_init(s_pfs); mtp_store_sd_init();` log `mtp_pfs_blocked_writes()`.
7. Start the USB code with `mtp_store_pfs_get()` and `mtp_store_sd_get()`. Loop forever.

Because the branch is before `chain_start()`, `ads1115_start()`, and the other starts, none of them runs. A second guard exists in the backends: `mtp_pfs_mount_readonly()`, `mtp_store_pfs_init()`, and `mtp_store_sd_init()` fail when `io_usb_files_mode()` is false. If MODE2 is read wrong (floating), the board runs as the serial DAQ, as today.
Test builds: `DAQ_SIM`, `DAQ_TOGGLE`, `DAQ_NANDREAD` run their own code before or inside `app_main`. Put the branch before `nand_read_test()` too, or ignore files mode in those builds.

## MTP-S-7 Changes needed in the existing code (not made; for the USB agent and the orchestrator)

`main.c`
* the files-mode branch of MTP-S-6; keep `s_pfs`, `scan_task`, `led_task` shareable (they are `static` now).
* `led_task`: `err` includes `(!sensor && n > 60)`: in files mode it would light the ERROR LED after 3 s. Add a files-mode flag that skips the sensor and `nau7802_fault` terms. `LED_RECORDING` and `LED_AUTOREC` stay off.
* the `DAQ_SIM` build: nothing.

`io.c` / `io.h`
* add `io_rec_disable()`. Optionally make `rec_set(true)` do nothing in files mode (a second guard).

`cmd.c` (only if a CDC command channel stays in files mode)
* `cmd_start()` installs `usb_serial_jtag` and `usb_serial_jtag_vfs_use_driver()`: do not call it in files mode. `cmd_task` reads with `usb_serial_jtag_read_bytes`, `do_daqs` and `usb_put` write with `usb_serial_jtag_write_bytes`, and all replies use `printf`. These four places need a small transport layer (function pointers `rd`, `wr`) so that the CDC port of TinyUSB can serve the same commands.
* In files mode the safe subset is: `PING`, `INFO`/`STATUS`, `LIST`, `FILES`, `SDLS`, `CHUNK`, `CHECK`, `DUMP`, `COMPARE`, `LED`, and a new `REBOOT` (see below). Refuse: `REC ON`, `PFSFLUSH`, `FLUSH`, `CAL SET`, `WIFI`, `TELEM`, `STREAM`, `MATLAB PING` (no stream exists).
* `REBOOT` calls `pfs_flush(2000)` and `sd_log_flush(2000)` and refuses when `rec` is on. In files mode skip both flushes (no writer: `pfs_flush` would only wait for its time-out) and restart at once. This is the software way back that STATE.md asks for.
* `do_chunk` (SD branch) and `do_files`/`do_sdls` test `sd_log_mounted()`, which is false in files mode. Use `mtp_store_sd_get()->present()` instead (the path is the same `/sdcard`).
* `do_info` prints `sd_log_*` counters (all 0 in files mode) and PFS counters: fine. Add `mode=files` and `blocked_writes=` to the line.
* `do_chunk`/`do_files` use `pfs_read`/`pfs_file_*` directly: they work as they are (read only), with the PFS mounted by the shim.

## MTP-S-8 Tests and build

* Build: `~/.platformio/penv/bin/pio run -e daq` in the worktree: SUCCESS, RAM 50.0 %, Flash 25.3 % of the app partition, 0 warnings from the new files. Not flashed.
  The system `pio` (`/usr/bin/pio`, Python without `intelhex`) compiles but fails at `bootloader.bin` with `ModuleNotFoundError: No module named 'intelhex'`. Cause: the PlatformIO core upgrade (6.2.0 / 6.2.1b2) and a newer `tool-esptoolpy`. The penv `pio` has the module. Other agents should use the penv `pio`.
* Host test: `tools/mtp_store_test.sh` builds `src/mtp_store_pfs.c` with `-Wall -Wextra -Werror -DMTP_HOST_TEST` against a fake PFS and a fake NAND. It checks: the real driver gets no write, erase, or mark-bad call and no NOR write (the fake asserts); a trimmed file starts at `start`; sequential and random reads match; the short read at the end; a corrupt page gives `0xFF` filler and is counted; one NAND read per page in a sequential read. Result: PASS. The SD backend has no host test (it needs FatFS and a card); it is reviewed only.
* `tools/` files are helpers, not deliverables.

## MTP-S-9 Risks

| Id | Risk | Level | Mitigation |
|---|---|---|---|
| R1 | A NAND write at the mount (MTP-S-2.4) | high | HAL shim (MTP-S-2.5); no writer task; counter. Hardware test of the shim: not done. |
| R2 | `nand->init()` clears the chip block-protect bits and sets the ECC feature (register writes) | low | Not array writes. Only the shim stops a write command after this. Persistence of the bits: UNVERIFIED. |
| R3 | A damaged NOR store is formatted by the stock NOR HAL (`format_if_mount_failed = 1`) | low | NOR flash only, not the NAND. It loses only the block-table journal (the mount uses Path B). A custom NOR HAL with no format would remove it. |
| R4 | Path B mount (NOR stale or empty) takes longer than Path A, and the NOR commit is refused | low | Slower mount only. Data is the same. The next normal boot repairs the NOR store. |
| R5 | The file index is not ready when the host asks for the list (about 3.6 s after boot) | medium | `refresh()` waits up to 10 s. The MTP layer should call `refresh()` at session open, not at boot. |
| R6 | Read time of a NAND page is not measured | medium | Measure with a read-only run (`pfs_read` loop on the board). Use a read-ahead task so the TinyUSB callback never waits on SPI. |
| R7 | MTP asks 512 or 1024 bytes at a time | low | The page cache gives one NAND read per page. |
| R8 | A page cannot be read (RS fails) | low | `0xFF` filler, counted, offsets stay true. The MTP layer must not stop the transfer. |
| R9 | The reported size of an approximate file can be larger than the real data | low | The tail is `0xFF` filler; `size_exact` is false. |
| R10 | The file name shows the current ADDR pins, not the recording board | low | Same rule as `daq_get.py`. |
| R11 | More than 128 PFS files | low | The PFS index drops the oldest entries (the ring has overwritten them in most cases). |
| R12 | The PFS version in use (v0.3.1) is not the repository head (0.4.0) | low | The API and the mount write paths are the same. The `pfs_status_t` difference is handled with `PFS_VERSION`. Re-check if `idf_component.yml` moves to 0.4.0. |
| R13 | SD card pulled out during a transfer | medium | `fopen`/`fread` errors drop the mount; `present()` mounts again. The MTP layer should send a storage-removed event. |
| R14 | `sd_log_start()` or `cmd_start()` started by mistake in files mode | high | They are not reachable if the branch is before `chain_start()`. Add a runtime assert `!io_usb_files_mode()` at the top of `sd_log_start()` and `cmd_start()`. |
| R15 | MODE1 high at boot starts a recording in files mode | high | `io_rec_disable()` before any other start; the writer task does not exist, so even `rec = true` cannot write. |
| R16 | The MODE2 pin floats in a noisy place | low | Pull-up gives serial mode. The 50 ms settle is in `io_init()`. |

## MTP-S-10 Not verified, and open points

* UNVERIFIED: the real mount with the shim on the board; NAND page read time; SD behaviour with a real card; persistence of the NAND protect bits after `init()`.
* Question for the orchestrator: names with or without the `NAND_` and `uSD_` prefix (MTP-S-4).
* Question: should the NOR HAL be a custom read-only HAL (no format) instead of the stock one (R3)? About 40 lines. Not done.
* Next steps: the USB agent calls `mtp_store_pfs_get()` and `mtp_store_sd_get()`, and calls `refresh()` at session open. The `main.c` branch and the `io.c`, `cmd.c` changes of MTP-S-7 are for whoever owns those files.
