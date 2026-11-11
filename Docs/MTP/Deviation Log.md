# MTP Deviation Log

Each row records a change from the old behaviour. Date: 2026-11-11. Branch: `mtp-usb-mode`.

| Date | What changed against the old behaviour | Why | Effect |
|---|---|---|---|
| 2026-11-11 | MODE2 low was "files mode, not used yet" in `src/board.h` and README. Now the `daq_mtp` build uses MODE2 low as files mode. | Files mode (USB MTP, read-only) is in development. | Only the `daq_mtp` build changes. The `daq` build ignores MODE2 as before. |
| 2026-11-11 | `src/cmd.c` and `src/stream.c` call `cio_*` functions instead of direct `usb_serial_jtag_*` calls. | One transport layer lets files mode swap the console backend. | Same bytes on USB Serial/JTAG in the `daq` build. `cmd_start()` installs the driver through `cio_init()`. |
| 2026-11-11 | In the `daq_mtp` build, `REC ON` is refused. MODE1 high does not auto-record. | No data may be written while the NAND is read-only for MTP. | `REC ON` replies `REC:ERR recording is disabled in this build (daq_mtp)`. No file is saved. |
| 2026-11-11 | Files mode runs no sensors, no chain, no WiFi and no recording. | Files mode only reads the store. | The board only serves files. Sensor and radio code do not start. |
| 2026-11-11 | The `daq_mtp` build sets the task watchdog to 8 s with panic (`CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=8`) and `CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y`, in both modes. The `daq` build keeps 5 s and no panic. | A hung USB stack must reset the chip with no human (Spike_Report.md MTP-P-10). A panic print to a dead console could block the reset. | In `daq_mtp` serial mode a watchdog or panic reset is silent (no backtrace on the console). `daq` is unchanged. |
| 2026-11-11 | `daq_mtp` files mode has the CDC commands `DL`, `REBOOT`, `LEAVE`, `LOG` and (test only) `HANG`. They are read by the USB code itself, not by the `cmd` task. | The way back from files mode must work when the `cmd` task is busy (Merge_Report.md MTP-M-5). | Only in `daq_mtp`. The `daq` build has none of them. |
| 2026-11-11 | New folder `mtp_usb/` (esp_tinyusb fork), new lock file `dependencies.lock.daq_mtp`, new env `daq_mtp_force`. The top `CMakeLists.txt` has a block for `-DDAQ_MTP_USB=ON`. | The USB stack must not touch the other envs (Merge_Report.md MTP-M-2). | `daq` image: the same loadable sections and symbols as before (only the build date field differs). |
