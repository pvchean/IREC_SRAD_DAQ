# MTP Deviation Log

Each row records a change from the old behaviour. Date: 2026-11-11. Branch: `mtp-usb-mode`.

| Date | What changed against the old behaviour | Why | Effect |
|---|---|---|---|
| 2026-11-11 | MODE2 low was "files mode, not used yet" in `src/board.h` and README. Now the `daq_mtp` build uses MODE2 low as files mode. | Files mode (USB MTP, read-only) is in development. | Only the `daq_mtp` build changes. The `daq` build ignores MODE2 as before. |
| 2026-11-11 | `src/cmd.c` and `src/stream.c` call `cio_*` functions instead of direct `usb_serial_jtag_*` calls. | One transport layer lets files mode swap the console backend. | Same bytes on USB Serial/JTAG in the `daq` build. `cmd_start()` installs the driver through `cio_init()`. |
| 2026-11-11 | In the `daq_mtp` build, `REC ON` is refused. MODE1 high does not auto-record. | No data may be written while the NAND is read-only for MTP. | `REC ON` replies `REC:ERR recording is disabled in this build (daq_mtp)`. No file is saved. |
| 2026-11-11 | Files mode runs no sensors, no chain, no WiFi and no recording. | Files mode only reads the store. | The board only serves files. Sensor and radio code do not start. |
