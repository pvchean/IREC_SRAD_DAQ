# MTP Merge Report (spike design merged into the real firmware, build only)

Author: MTP MERGE agent. Date: 2026-11-11 (about 03:00 to 04:00). Branch `mtp-usb-mode`. Language: ASD-STE100. Labels: MTP-M-n.
No hardware was used. Nothing was flashed. No /dev/ttyACM* was opened. Zero NAND writes (the code has no NAND write path, MTP-M-8).

## MTP-M-1 Result in one table

| Item | State |
|---|---|
| `pio run -e daq_mtp` | SUCCESS, no warning (clean build from nothing, 44 s). RAM 64.7 % static (was 51.9 %), flash 27.7 %. |
| `pio run -e daq_mtp_force` (new test env) | SUCCESS, no warning. |
| `pio run -e daq` | SUCCESS, no warning. Same loadable sections, same symbols (names and sizes), same `sdkconfig.daq`, same `dependencies.lock` as before the merge. Only the build date field of the app descriptor differs (82 bytes of the .bin). No tinyusb, mtp_vfs or usb_files symbol in the daq ELF. |
| `tools/mtp_store_test.sh` | PASS (blocked NAND 3, NOR 1). |
| Strong `files_mode_usb_start` | `nm` shows `T` (strong). The weak default of `files_mode.c` is not linked. |
| MTP callbacks of the fork | Linked after a fix (MTP-M-4). Before the fix they were NOT linked, in the spike too. |
| Hardware test | NOTHING tested. See MTP-M-7 and MTP-M-9. |

## MTP-M-2 How the build works (daq_mtp only)

1. `platformio.ini`, env `daq_mtp`: `board_build.cmake_extra_args = -DDAQ_MTP_USB=ON` (CMake option) and `-DDAQ_MTP=1 -DDAQ_MTP_USB=1` (C macros).
   Env `daq_mtp_force` extends it and adds `-DDAQ_MTP_FORCE_FILES=1` (files mode with no MODE2 switch, MTP-M-7).
2. Top `CMakeLists.txt`: only when `DAQ_MTP_USB` is set it (a) adds `mtp_usb/` to `EXTRA_COMPONENT_DIRS`, (b) sets `SDKCONFIG_DEFAULTS` to
   `sdkconfig.defaults` then `mtp_usb/daq_mtp.defaults`, (c) sets the build property `DAQ_MTP_USB` and (d) sets the build property `DEPENDENCIES_LOCK`
   to `dependencies.lock.daq_mtp`. Other envs skip the block: they never see the fork, the extra sdkconfig, or the second lock file.
3. `mtp_usb/` is a component with no source. Its `idf_component.yml` has the dependency `espressif/esp_tinyusb 3.0.0` with `override_path: fork/esp_tinyusb`.
   The component name of the fork is `esp_tinyusb`. Its folder is `mtp_usb/fork/esp_tinyusb` (not named `components/`).
4. `src/CMakeLists.txt`: when the build property is set, it adds `usb_files.c` and `mtp_vfs.c` to the sources and `mtp_usb esp_tinyusb vfs esp_hal_usb`
   to PRIV_REQUIRES. The property (not a plain CMake variable) is needed because IDF reads the component requirements in an early script that has
   build properties but no project variables.
5. PlatformIO compiles the project sources (`src/`) itself and links them as object files. So the strong `files_mode_usb_start` in `usb_files.o` wins against
   the weak one in `files_mode.o`. No WHOLE_ARCHIVE is needed for that (and PlatformIO does not keep a per-library `--whole-archive`: see MTP-M-4).
6. Rules for the next agent:
   * After you edit `mtp_usb/daq_mtp.defaults`, delete `sdkconfig.daq_mtp` (and `sdkconfig.daq_mtp_force`). After you edit a CMake file inside `mtp_usb/`,
     run `touch CMakeLists.txt` (PlatformIO only watches the two CMakeLists of the project).
   * Use `~/.platformio/penv/bin/pio`.

## MTP-M-3 File list

New:
* `mtp_usb/CMakeLists.txt`, `mtp_usb/idf_component.yml`, `mtp_usb/daq_mtp.defaults`: the dependency carrier and the sdkconfig of `daq_mtp`.
* `mtp_usb/fork/esp_tinyusb/`: copy of `tools/mtp_spike/fork/esp_tinyusb` (read-only MTP, plain VFS storage, P1 mtp_device recovery, P2 watchdog feed in the USB task)
  with two new changes (MTP-M-4): the session-open hook and the link anchor. 304 files, 4.9 MB. The spike still has its own copy (not changed).
* `dependencies.lock.daq_mtp`: lock file of `daq_mtp` (identical after a clean rebuild).
* `src/usb_files.{h,c}`: TinyUSB start, CDC console backend, grace window, pad safety, way back, log ring (whole file is `#ifdef DAQ_MTP_USB`).
* `src/mtp_vfs.{h,c}`: the read-only VFS shim at `/daq` over the two stores.
Changed:
* `src/files_mode.{h,c}`: new `files_mode_restart(FM_REBOOT | FM_DOWNLOAD | FM_LEAVE)`; `files_mode_leave()` uses it. The old code stays in the `#else` branch (`daq`).
* `src/cmd.c`: files-mode `REBOOT` and `LEAVE` call `files_mode_restart()`; new `DL`. Under `#ifdef DAQ_MTP`; the `#else` branch is the old code.
* `src/main.c`: `usb_files_early_init()` first in `app_main()`, `usb_files_log_start()` at the start of `files_mode_main()` (both `#ifdef DAQ_MTP_USB`).
* `src/io.c`: `DAQ_MTP_FORCE_FILES` (only with `DAQ_MTP`).
* `CMakeLists.txt`, `src/CMakeLists.txt`, `platformio.ini`, `Docs/MTP/Deviation Log.md` (three rows added).
Unchanged on purpose: `src/mtp_store*.{h,c}` (the stores), `src/nor_ro.c` (read-only NOR HAL), `src/console_io.c`, the PFS mount wrapper `mtp_pfs_mount_readonly()`.
Commits: 85e715a (fork copy), e4464b3 (build wiring), d53a658 (source), b77b0d3 (force env, deviation log). This report is committed after them.

## MTP-M-4 Finding: the fork's MTP callbacks were not linked (also in the spike)

* In `firmware.elf` of the spike (`spike_force`) and in my first `daq_mtp` build, `nm` showed `W tud_mtp_command_received_cb` and the other `tud_mtp_*_cb` as WEAK.
  The strong versions are in `fork/esp_tinyusb/mtp/tinyusb_mtp_transport.c`. The weak defaults in `tinyusb/src/class/mtp/mtp_device.c` return -1.
  The linker takes an archive member only for an undefined symbol. The weak default already defined the symbol, so `tinyusb_mtp_transport.o` was never pulled.
  The symbol `mtp_dispatch` (in that object) was not in the ELF.
* Effect: every MTP command (OpenSession too) got a failure answer from the weak default. This is a very likely root cause (or part of the root cause) of
  `PTP_ERROR_IO: failed to open session` in MTP-P-9.3. The Opus hint (Opus_Hint.md) did not see it. The P1 back-port could not act on the real handlers.
  Nobody has tested the real handlers on hardware yet.
* Fix (in the new fork copy only): `tinyusb_mtp_install_driver()` calls `mtp_transport_link_anchor()`, an empty function in `tinyusb_mtp_transport.c`. The
  call pulls the object. After the fix `nm` shows `T` for `tud_mtp_command_received_cb`, `tud_mtp_data_xfer_cb`, `tud_mtp_data_complete_cb`,
  `tud_mtp_request_cancel_cb`, `tud_mtp_request_device_reset_cb`, `tud_mtp_request_get_device_status_cb`, and `T mtp_dispatch`.
  Still weak and unused: `tud_mtp_request_get_extended_event_cb`, `tud_mtp_request_vendor_cb`, `tud_mtp_response_complete_cb` (the fork has no strong version).
  The CDC and the `tud_mount_cb` family are strong (checked).
* `WHOLE_ARCHIVE` in the fork CMake was tried first. CMake writes it correctly, but PlatformIO moves all `--whole-archive` flags out of their place: no effect. Removed.
* The spike fork copy in `tools/mtp_spike` still has the bug. If the spike is used again, copy the three small changes (`tinyusb_mtp_transport.{c,h}`, `tinyusb_mtp.c`).
* CHECK for the next agent on every build: `xtensa-esp32s3-elf-nm -C .pio/build/daq_mtp/firmware.elf | grep tud_mtp_` must show `T` for the six callbacks above.

Other fork change (session-open hook): `tinyusb_mtp_set_session_open_callback(cb)` (`include/tinyusb_mtp.h`). `mtp_op_open_close_session()` calls `cb` at OpenSession,
without the MTP lock, before it sets `session_open` (and checks again after). Used for the refresh (MTP-M-5.4).

## MTP-M-5 Runtime design

### MTP-M-5.1 Boot
* `app_main()` (daq_mtp builds, both modes): `usb_files_early_init()` = `usj_pads_restore()` (writes only if `USB_SERIAL_JTAG.conf0` is not at the reset default) then `mux_to_jtag()`.
  This is the v3 code of MTP-P-11, copied. The v1 pad disconnect and the crash-reset disconnect (`SPIKE_P3`) are NOT merged.
* MODE2 low (or `daq_mtp_force`): `files_mode_main()`: log ring starts, no recording, LED task, NAND bind, read-only NOR HAL, `mtp_pfs_mount_readonly()` (10 tries),
  PFS scan task, store init (PFS, SD), `cmd_start()` (the cmd task, on a stub console), `files_mode_usb_start()`.
* `files_mode_usb_start()` (strong, never returns): grace window 10 s (`USB_GRACE_MS`) with the USB Serial/JTAG alive (303a:1001, a plain `pio upload` works; log lines
  "files mode: USB MTP starts in N s"). The window starts after the NAND mount, so the first 1001 appears at power-up and stays about 10 s plus the mount time.
  Then: `mtp_vfs_init()`, hook set, MTP driver, one storage "DAQ" at `/daq`, TinyUSB (task stack 6144), CDC, `cio_set_backend()`. The descriptors and sdkconfig keys
  are those of the spike (CDC + MTP, 4 IN endpoints used of 5: do not add a class).
* The USB task feeds the task watchdog (fork P2). `daq_mtp.defaults`: 8 s, panic, silent reboot (as the spike). It applies to both modes of `daq_mtp` (Deviation Log).

### MTP-M-5.2 CDC console (existing allowlist works over CDC)
`cio_set_backend()` gets a CDC backend: write drops the data when no host has the port open (DTR low) so the cmd task never blocks; read comes from a stream buffer filled by the USB task.
The files-mode allowlist of `cmd.c` serves: PING, LED, LIST, FILES, SDLS, CHUNK, CHECK, DUMP, INFO/STATUS. Everything else: `ERR not available in files mode (read-only)`.

### MTP-M-5.3 The way back (read by the USB code itself, not by the cmd task)
The CDC receive callback reads whole lines. These five lines act at once, do not go to the cmd task, and work when the cmd task is hung:
`DL` (ROM download mode), `REBOOT` (files mode again), `LEAVE` (serial mode for one boot), `LOG` (dump of the 6 KB log ring, then `END`), `HANG` (TEST ONLY: block the USB task; the 8 s watchdog must reset the chip).
Every other line goes to the cmd task. `DL`, `REBOOT`, `LEAVE` also exist in the `cmd.c` allowlist (same function, `files_mode_restart()`).
`files_mode_restart()`: only the first caller acts; waits 300 ms for the reply; `usb_files_prepare_restart()` = `tud_disconnect()` + 500 ms (only if TinyUSB runs), `mux_to_jtag()`, `usj_pads_restore()`;
for `DL` it writes `RTC_CNTL_OPTION1 = FORCE_DOWNLOAD_BOOT`; `esp_restart()`. This is the path proven in MTP-P-9.2 (DL gave 303a:1001 in 4 s).

### MTP-M-5.4 Storage shim (`mtp_vfs.c`)
* One VFS `/daq` with two folders: `NAND` and `SD`. Names come from the stores: `NAND_F<id>D<dev>.BIN` (the spec says `NAND_F<hex>.BIN`; the storage agent's name keeps the device ID), `uSD_<8.3 name>`.
* Sizes are exact (`size` of the store; `size_exact` false means the PFS index size, see Storage_Design MTP-S-3). Random read: `store->read(index, offset, buf, len)`. A page that PFS cannot read is 0xFF filler (counted by `mtp_store_pfs_lost_pages()`).
* Snapshot: `mtp_vfs_refresh()` runs the store `refresh()` calls in a helper task (stack 8192) and copies name, size, time into tables (128 NAND, 256 SD). It is called ONLY from the OpenSession hook, never at boot.
  The USB task waits in 250 ms steps and feeds the task watchdog (30 s at most), so a slow refresh (PFS index wait up to 10 s, SD mount try) cannot trigger the 8 s watchdog. After a refresh the old file handles fail with EIO.
* SD absent: `refresh()` gives -1, the folder `SD` is empty. A store that is not bound: empty folder. NAND time is a fixed date (2026-01-01).
* All write calls (open for write, write, unlink, rename, mkdir, rmdir) fail with EROFS. The fork refuses the MTP write operations too (0x200E/0x200F) and does not list them in DeviceInfo.
* One mutex covers all shim state and the store calls.

## MTP-M-6 Differences from the spike

| Spike | Merged firmware |
|---|---|
| Fake VFS (3 MB and 100 KB pattern files) | Real stores through `mtp_vfs.c`; names `NAND_F...`, `uSD_...` |
| `INFO` answered by the spike | `INFO` is the real files-mode `STATUS:OK mode=files ... blocked_writes=... lost_pages=...` (needs the cmd task) |
| Grace window in `app_main` | Grace window in `files_mode_usb_start()` (after the NAND mount) |
| `HANG`, `ABORT`, `LOG`, `DL`, `REBOOT` in the CDC callback | `DL`, `REBOOT`, `LEAVE`, `LOG`, `HANG` (no `ABORT`); acts run in the main task, not in the callback |
| `SPIKE_P3` (crash-reset disconnect, off) | Not merged. After a watchdog reset the host keeps a stale device: a replug is needed (MTP-P-11.5) |
| `esp_log_set_vprintf` ring from `app_main` | Ring starts at the start of `files_mode_main()` in files mode only |
| No OpenSession hook | Hook + refresh (MTP-M-5.4) |
| Fork without link anchor | Fork with link anchor (MTP-M-4) |
| TWDT config only in the spike | TWDT panic 8 s for both modes of `daq_mtp` |
| Serial `SPIKE_FORCE_FILES` env | Env `daq_mtp_force` |
| MTP serial number "SPIKE0001" | NULL: the MAC based serial of the chip |

## MTP-M-7 Hardware test order for the next agent

Rules first: AGENT_RULES board-safety rules apply. The first test is a power cycle. Stop at the FIRST surprise and report. Never run `mtp-detect`, `usbreset`, or `mtp-files` before step 8. No `REC ON`, no `FORMAT`.
Board state at the time of writing (STATE.md): the board is probably invisible (spike v1 pad bug) and needs a human with the BOOT button. The real firmware has never run on it.
* Which env: `daq_mtp_force` for all tests (the board has no MODE2 switch; GPIO 2 reads high). Recording is locked in both envs. Do not flash `-e daq`.
* Before each boot of a test build make sure the MODE1 switch cannot start a recording: `daq_mtp*` ignores MODE1 (`io_rec_disable()`), so it is safe.

0. Human: hold BOOT, plug the cable in (full power loss first, 3 s). `lsusb` must show `303a:1001`. Agent: `~/.platformio/penv/bin/pio run -e daq_mtp_force -t upload --upload-port /dev/ttyACM0`.
   Human: unplug, wait 3 s, plug in WITHOUT BOOT. (Full power loss clears a dirty pad from the old firmware.)
1. Power-cycle proof: `303a:1001` must show for the grace time (the USB Serial/JTAG console prints "files mode: USB MTP starts in N s", `pio device monitor` at 115200 shows it).
   After about 10 s plus the mount time: `303a:4041` (`lsusb -v`: IAD, CDC + interface 2 class 6/1/1). If 1001 is missing: STOP, ask the human for BOOT + replug.
2. CDC with pyserial on `/dev/ttyACM0` (`tools/mtp_spike/host_cdc.py PORT CMD [seconds]` works): `INFO`. Expect `STATUS:OK mode=files rec=0 mounted=1 index=1 blocked_writes=N nor_blocked=M lost_pages=0`.
   `blocked_writes`/`nor_blocked` above 0 means PFS tried to write at the mount (Storage_Design MTP-S-2.4): the shim refused it. Record the numbers. `LOG`: expect "USJ pads were clean" (DIRTY on the very first boot is also fine, report it) and the NAND mount lines.
   Read-only checks: `LIST`, `FILES` (names and sizes of the real files).
3. `DL`: expect `303a:1001` within about 4 s. Flash the same env again through it (proves the way back). Then a power cycle and step 1 again.
4. `REBOOT`: expect a disconnect, then 1001 for the grace, then 4041. `LOG`: reset reason is software, pads clean.
5. `LEAVE`: expect 1001 and the serial DAQ (`PING` answers on USB Serial/JTAG, MODE1 does not record). Then a power cycle to go back to files mode.
6. `HANG` (optional, the human must be at the board): the chip resets after 8 s. The host keeps a stale 4041 (P3 is not merged): a replug is expected. After the replug repeat step 1. The WDT proof of MTP-P-10.2 stands, so you may skip this.
   Do not go on if 3 to 5 failed.
7. MTP stages with the raw client `tools/mtp_spike/host/ptp_probe.c` (build: `gcc -O2 -Wall -o $SCRATCH/ptp_probe tools/mtp_spike/host/ptp_probe.c -lusb-1.0`).
   It was written for the FAKE files (F00001.BIN, 3 MB pattern): against the REAL storage only stages 1 to 3 make sense. Run `-c /dev/ttyACM0 -s 1`, then `-s 2`, then `-s 3`.
   Stage 3 prints the real object names and sizes, then reports "F00001.BIN or F00002.BIN missing" as a FAIL: that FAIL is expected. Check the NAND file names and sizes against `FILES` from step 2.
   Stages 4 to 6 (pattern compare, speed) need a change: read a real file (small first) with GetObject and GetPartialObject and compare the bytes with `CHUNK`/`DUMP` over CDC (ground truth from PFS) or with `tools/daq_get.py`.
   Stage 7 (SendObjectInfo and DeleteObject must be refused with 0x200E/0x200F) is safe on real data (the shim and the fork refuse; nothing can be written). Run it after the read tests; if the bulk pipe stalls, use `-r` (DEVICE_RESET).
   If the board stops answering, the 8 s watchdog resets it: CDC `INFO` shows the uptime. If the host shows a stale 4041 after a watchdog reset: replug.
8. Only after step 7 passed: `mtp-detect` once, then `mtp-folders`, `mtp-files`, `mtp-getfile` of one small file, then a file manager (Windows, macOS with OpenMTP). Compare a copied file with `daq_get.py` output.
9. Measure speed (GetObject of a large NAND file). Test the session refresh: close the session and open it again; the lists must stay the same, and `INFO` must still show the same `blocked_writes` (no write happened).

## MTP-M-8 NAND and commands: what is safe in this mode

* The NAND is read-only in files mode, in three layers: (1) `mtp_pfs_mount_readonly()` wraps the NAND HAL (`write_page`, `erase_block`, `mark_bad_block` return `NAND_ERR_PROTECTED`, counted) and the NOR HAL (`write_atomic` fails, counted);
  (2) `nor_ro.c` mounts LittleFS with `read_only=1` and `format_if_mount_failed=0`; (3) the files-mode boot starts no `pfs_writer`, no sensors, no `pfs_ingest`/`pfs_flush`, no format; `io_rec_disable()` locks recording (`rec_set(true)` does nothing; `REC ON` is refused).
* The MTP layer reads files with `fopen`/`fread` on the shim only. The shim has no write path.
* Safe commands over CDC (read only): PING, LED, LIST, FILES, SDLS, CHUNK, CHECK, DUMP, INFO/STATUS, LOG. Way back: DL, REBOOT, LEAVE. Test only: HANG.
* Everything else is refused in files mode (`ERR not available in files mode (read-only)`): REC, FORMAT, PFSFLUSH, FLUSH, CAL, WIFI, TELEM, STREAM, MATLAB, and so on.
* The `daq_mtp` serial mode (MODE2 high, or after LEAVE) runs the DAQ with recording locked. The writer task runs but has no data. Do not use `daq` on a board that holds data you want to keep (MODE1 high there records at boot).

## MTP-M-9 Not tested on hardware, and open risks

Nothing was tested on hardware. This includes everything that the spike never proved: the real MTP handlers (MTP-M-4: they never ran), OpenSession, GetStorageInfo with `total_bytes`,
GetObjectHandles on the shim, GetObject, GetPartialObject, the write refusals, libmtp, Windows, macOS, speed, and CDC while MTP transfers run.
Also untested: the CDC backend and the line parser, the log ring dump, `files_mode_restart()` from the cmd task, the refresh helper task, the read-only mount of the real NAND in the `daq_mtp` image, and the SD path (no card is installed).
Risks:
1. `tinyusb_mtp_transport.o` is now linked for the first time. Its code paths (P1 recovery code too) are untested. A hang there ends in the 8 s watchdog and a silent reboot.
2. After any watchdog or panic reset the host shows a stale device (no P3). A replug is the way out. Keep the BOOT button reachable.
3. The SD store and the cmd task share state: `cmd.c` SDLS/CHUNK calls `mtp_store_sd` while the MTP task may read the same store (the store rule is "one caller at a time"). Do not run SDLS/CHUNK on SD during an MTP transfer. Same for the PFS one-page cache (cmd.c reads PFS directly, so no shared cache there; PFS has its own mutex).
4. The refresh waits up to 30 s with the watchdog fed. The host may time out the OpenSession earlier (libmtp default is about 10 s?). A normal refresh is fast (index ready, no card): unmeasured.
5. RAM: static RAM 64.7 % (MTP object table 512 entries, buffers, shim tables about 21 KB). Check `INFO`/heap in files mode. The NAND page cache and PFS scan add to it.
6. Full-speed USB (12 Mbps, 64 byte bulk): MTP reads will be slow (hundreds of KB/s at most). Not a bug.
7. `daq_mtp` serial mode has a panic-silent watchdog (Deviation Log): debug serial mode with `daq`, not `daq_mtp`.
8. `usb_files.c` fixes the grace window at 10 s after the mount. If the mount takes long, the first 4041 appears late. Not a risk for flashing (USB Serial/JTAG is alive during all that time).
9. The device name list uses the PFS file names `NAND_F<id>D<dev>.BIN` (the task text says `NAND_F<hex>.BIN`): decide with the user before the final summary.
10. The spike fork copy (`tools/mtp_spike`) is stale (no link anchor, no hook). Two copies of 4.9 MB are in git.

Approximate tokens of this agent: 300k.
