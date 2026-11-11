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

## Status
(nothing done yet)
