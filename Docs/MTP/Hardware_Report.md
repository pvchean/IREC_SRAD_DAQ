# MTP Hardware Report

Author: MTP HARDWARE agent. Date: 2026-11-11. Branch `mtp-usb-mode`. Language: ASD-STE100. Labels: MTP-HW-n.
Zero NAND writes. No MTP command was sent. `LEAVE`, `REC ON`, `usbreset`, `mtp-detect` were not used.

## MTP-HW-1 Step 1: identify the running firmware

* Command: `python3 tools/mtp_spike/host_cdc.py /dev/ttyACM0 INFO 3`.
* Reply: `INFO: spike files mode, up 75 s, heap 351040`. This is the old spike firmware (v1 or v3: the reply does not say which). It has no NAND code.
* Result: OK.

## MTP-HW-2 Step 2: get daq_mtp_force on the board

* `tools/mtp_hosttest/run.sh`: 8427 checks passed, 0 failed.
* `pio run -e daq_mtp_force`: SUCCESS (RAM 64.7 %, flash 27.7 %). `nm -C .pio/build/daq_mtp_force/firmware.elf | grep tud_mtp_`: `T` for the 6 callbacks and `tud_mtp_data_*`/`response_send`.
* CDC `DL` was sent to the running spike firmware (`host_cdc.py /dev/ttyACM0 DL 2`).
  The port closed at once (pyserial: "device reports readiness to read but returned no data").
* Then the agent polled `lsusb | grep 303a` for about 15 s: NO 303a device appeared, no /dev/ttyACM*, no /dev/ttyUSB*.
* ANOMALY (as the task predicted for the old v1): the board is gone after `DL`. Rule applied: STOP, do nothing else.
* Result: FAIL. The board is invisible. It needs a human: hold BOOT, unplug and plug in the USB cable (full power loss first, wait 3 s),
  check `lsusb` shows `303a:1001`, then run `~/.platformio/penv/bin/pio run -e daq_mtp_force -t upload --upload-port /dev/ttyACM0`
  (the build is ready in `.pio/build/daq_mtp_force`), then unplug, wait 3 s, plug in WITHOUT BOOT.

## MTP-HW-3 Not done (blocked by MTP-HW-2)

Steps 3 to 8 (power-cycle proof, INFO/LOG DIAG, REBOOT, DL reflash, HANG, ptp_probe, libmtp) were not run.
Nothing of the real firmware has run on hardware yet.
