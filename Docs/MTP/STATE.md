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
