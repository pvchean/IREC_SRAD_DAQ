# MTP project state (the orchestrator edits this file; agents read it)

Started: 2026-11-11 00:00 EST. Deadline: summary due about 10:00-11:20 EST same day. User needs 40 % of a 5 h window from 12:00.
Windows: A = until 01:20, B = 06:20 to 11:20. Pause at about 01:05 (window A), resume at 06:20, stop new work at about 11:00.
Branch: mtp-usb-mode (worktree .claude/worktrees/mtp). Base commit e9afd64.

## Known up front
* Project is ESP-IDF, not Arduino. TinyUSB (upstream) has an MTP class driver (CFG_TUD_MTP, since 0.19) - to be checked.
* ESP32-S3 has one USB PHY: USB Serial/JTAG and USB OTG (TinyUSB) cannot both be active. Files mode must use TinyUSB; serial mode
  keeps USB Serial/JTAG. So the mode is chosen at boot from MODE2. A composite device (MTP + CDC) can keep serial I/O in files mode.
* macOS has no built-in MTP driver. "Works on macOS" needs a third-party tool (OpenMTP, Android File Transfer) - verify in research.
* Flash/serial recovery: when TinyUSB owns the USB pins, /dev/ttyACM0 is the CDC port (or gone). To reflash, hold BOOT and reset (ROM
  download mode) - the user is away, so the firmware MUST keep a software way back (CDC command `REBOOT`, or esptool via CDC 1200-bps/DTR-RTS auto-reset).

## Ledger (orchestrator keeps it; approximate tokens)
| time | agent | model | task | tokens | result |
|---|---|---|---|---|---|
| 00:05 | research | sonnet | MTP options | ~280k | done: Research.md (commit dcbd15c). Plan A: esp_tinyusb 3.0.0 + local fork + read-only VFS shim |
| 00:15 | storage | sonnet | PFS read API, mtp_store, build | ~190k | done (186b5c2,94be6cd,f84efd2). PFS mount can write NAND -> HAL wrapper blocks writes. File names NAND_F<hex>.BIN / uSD_F<hex>.BIN |
| 00:35 | integ | sonnet | boot branch, console_io, daq_mtp env, no-format NOR HAL | running | |
| 00:15 | spike | sonnet | standalone MTP+CDC on board | running | |

## Status
Decisions (orchestrator, 00:15): plan A. ONE MTP storage with root folders NAND and SD (user asked folders in root, not two drives).
VID/PID 303A/4041 ok. Leave files mode by CDC `REBOOT` / `DL` (ROM download). macOS = MTP + OpenMTP (option C, read-only MSC, only if time).
Open: does PFS mount write to NAND? (storage agent checks). `daq_mtp` build must disable recording before any flash of the real firmware.

Build: use ~/.platformio/penv/bin/pio (system pio lacks intelhex). Keep NAND_/uSD_ prefixes. Custom no-format NOR HAL wanted.

## Checkpoint 00:3x (window A ended early: user reported 5 % left; orchestrator had no usage meter and was late)
Agents told to pause and write Spike_Report.md / Boot_Integration.md. Window A total agent tokens ~ 480k + spike/integration (unknown, see their reports).
Resume: a session cron fires at 06:23 and starts fresh agents from these files. It lives only while the VS Code session stays open.

## Spike agent report (00:4x, ~170k tokens, commits 84fe009, 4fccca6)
* The board did NOT run the DAQ firmware. It ran a PFS-PIL test firmware ("@L:I:PFS-PIL READY 64 16 2048"). The spike overwrote
  bootloader, partition table and app only (pfs_nor and NAND untouched). That old firmware is gone; the user should know.
* Board now runs the SPIKE firmware (serial mode, MODE2 reads HIGH, recoverable with penv pio upload, no BOOT button).
  DECISION: leave the spike firmware on the board. Do NOT flash `-e daq` (auto-record at boot if MODE1 high = NAND writes).
* esp_tinyusb 3.0.0 builds on IDF 6.1 / platform 7.1.3. `override_path` fork works (do not name the folder `components/`).
  Fork: plain VFS as storage; read-only; write ops return 0x200E/0x200F. NOT host-tested yet.
* Untested and critical: lsusb, libmtp, GetPartialObject, speed, CDC `DL` and `REBOOT` (the way back from files mode).
* Next: Spike_Report.md MTP-P-7: flash `spike_force`, wait 10 s grace, lsusb -v; test REBOOT, then DL, before anything else.
* Ledger: window A agent tokens total ~ 190k+190k+280k+170k = ~830k (integration agent still unreported).

## Integration agent report (~200k tokens, WIP commit 73912f9)
* `-e daq` and `-e daq_mtp` build. Not flashed. Done: console_io (cio_*), files_mode, io_rec_disable + RTC force-serial flag,
  nor_ro (no-format read-only LittleFS NOR HAL), main.c files-mode branch (DAQ_MTP + MODE2 low), `daq_mtp` env (REC ON refused),
  files-mode command allowlist + LEAVE. Details: Boot_Integration.md.
* Not done: mtp_store_test.sh run, Deviation Log + README/board.h lines, weak-symbol link check (`nm` on the ELF; else WHOLE_ARCHIVE
  or #ifdef), real read-only mount on the board, `DL` command (spike has it; merge it).
* Window A agent total ~ 1.03M tokens (research 280k, storage 190k, spike 170k, integration 200k, + orchestrator).
* Next (window B): (1) spike host tests, REBOOT then DL first. (2) Merge the spike fork + CDC into the daq_mtp build: strong
  files_mode_usb_start(), cio_set_backend(CDC), mtp_store backends behind the VFS shim. (3) Test on Linux with the board, read-only.
  (4) Deviation Log, README. (5) Final summary at 10:45.

## 01:35 Haiku (low) report, commit 164a590
* `daq_mtp` builds; `files_mode_usb_start` is W (weak default) in the ELF. The weak->strong override test (MTP-B-6) is still TODO: do it when the USB code adds the strong symbol.
* tools/mtp_store_test.sh PASS (blocked NAND 3, NOR 1). Deviation Log.md created. README MODE2 line updated.
* 01:27 running: sonnet spike TEST agent (REBOOT/DL first, then libmtp, GetPartialObject, speed, write refusal, CDC alongside MTP).

## 01:45 Spike TEST report (~120k tokens, commits a6004ca, e7f0fde)
* PASS: spike_force flashes; lsusb: 303a:4041 FS, IAD, CDC ACM + PTP class 6/1/1 "IREC DAQ MTP"; CDC INFO works in files mode.
* Way back: REBOOT/DL left D+ pulled up (host saw a stale dead device). FIX: tud_disconnect() + 500 ms delay before restart
  (in tools/mtp_spike/src/main.c; must go into the real firmware). After the fix DL -> 303a:1001 in 4 s, esptool flash works, no BOOT.
* FAIL: libmtp mtp-detect finds the device but PTP_ERROR_IO "failed to open session"; libmtp's USB reset hung the firmware; the host
  xhci now says "device not accepting address", usb_hub_wq kworker stuck in D. NO /dev/ttyACM0, NO 303a in lsusb. No root -> cannot recover.
  >>> BOARD NEEDS A HUMAN REPLUG (or RESET) <<< Until then NO hardware tests. Board firmware: spike_force (with fix), hung in files mode.
  After a replug: 10 s grace on USB-Serial-JTAG; plain `pio run -e spike -t upload --upload-port /dev/ttyACM0` works in that window.
* Lesson: never run mtp-detect first. Use a raw libusb client (gcc+libusb headers exist, no pyusb) + CONFIG_TINYUSB_MTP_TRACE_WRITES + CDC LOG ring.
* Rule added: before any host-side MTP test, the firmware must have a hardware watchdog that resets the chip when the USB stack hangs.
* 01:45 started: OPUS static review (no hardware): Docs/MTP/Opus_Hint.md (root causes, test plan, watchdog self-recovery).
* Ledger window A+: haiku 43k, spike-test 120k (est. total since 00:00: ~1.2M agent tokens).

## 01:55 Opus hint (commit 23186a2, static review). Board is back (user replugged at ~01:49): 303a:4041, spike_force, files mode.
* Likely cause: TinyUSB 0.21.0 mtp_device.c recovery bugs (DEVICE_RESET status stage never answered, dropped ZLP/wrong-type packet leaves
  the bulk pipe dead, no busy check -> assert); debug level 1 assert prints to a dead console; dcd_dwc2 edpt_disable loops without timeout.
  Ruled out: 64 B vs 4096 B buffer, interrupt EP, FIFO size, VFS locks. IN endpoints at the limit (5): never add a 6th.
* Plan: patches P1 (back-port), P2 (task WDT panic, fed by USB task), P3 (D+/D- low at boot), P4 (sdkconfig). Prove WDT with a CDC `HANG` command
  BEFORE any MTP test. Then raw libusb client (no mtp-detect first, no usbreset).
* 01:55 running: sonnet FIX agent (does the above in that order).

## 02:15 FIX agent (commits f99bfc3, 76da66b): details in Spike_Report.md MTP-P-10
* P1, P2, P4 built; WDT PROVEN (HANG -> reset in 8 s -> host sees disconnect and 4041 again). ptp_probe.c written (compiles, not run).
* P3 v1 broke the USB Serial/JTAG pad: board is INVISIBLE (port "not attached"). HUMAN NEEDED: hold BOOT + replug, then flash spike_force (v3, fixed, untested).
  Recipe and test order: MTP-P-10.4. No MTP host test has run yet.
