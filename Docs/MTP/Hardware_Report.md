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

## MTP-HW-4 Orchestrator run, 2026-11-11 08:20-08:33 (the user held BOOT for the first flash)
Board: DAQ with NAND (usable_blocks 1019), no SD card. Firmware: `daq_mtp_force` (commit of this branch at that time).
| Step | Result |
| :-- | :-- |
| BOOT flash of daq_mtp_force | PASS. `ttyACM0` appears about 1 s after `303a:1001`. |
| Power cycle | PASS. `303a:1001` for 10 s, then `303a:4041`. |
| CDC `INFO` | `mode=files rec=0 mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0` |
| CDC `LOG` | NAND mounted read only (Path A). DIAG: heap_free 56904, min 53624; free stack TinyUSB 6804, mtp_vfs 7500, cmd 6560, main 1456, led 2120. |
| `REBOOT` | PASS. 10 s `1001`, then `4041`, no replug. |
| `DL` and reflash | PASS. `1001` in about 1 s, flash with no BOOT, boots to files mode. |
| ptp_probe stages 1-3 | PASS (OpenSession 30 ms; one storage "DAQ", read-only, id 0x00010001; root has NAND and SD). |
| ptp_probe stage 6 (writes) | PASS. SendObjectInfo and DeleteObject return 0x200E, FormatStore 0x2005. Device stays alive. |
| libmtp `mtp-detect`, `mtp-folders`, `mtp-files` | PASS. 18 operations incl. GetPartialObject and GetPartialObject64. |
| Real file, stages 4-5 | PASS. `NAND_F00002D4.BIN`, 323696 bytes, CRC32 f3d49e4f twice; 20 random GetPartialObject reads match (lengths 1 to 65536, page crossings). Speed 767 KB/s. |
| libmtp `mtp-getfile` | PASS. Same size and CRC32 f3d49e4f. `bin_to_csv.py` decodes 40450 samples (PT sawtooth, 1 ms steps). |
Not tested: `HANG`, `LEAVE` (forbidden), SD card (not installed), Windows, macOS, a desktop file manager (gvfs/Dolphin), a transfer longer than 1 MB.
Note: `SendObjectInfo` refusal takes 2 s in the probe because the probe waits for an early response before it sends the data phase.

## MTP-HW-5 NAND write log (the user approved a 30 s test recording at 08:2x, limit for the task 500 Mbit)
* Why: the NAND had no files, so MTP reads could not be tested.
* How: env `daq_sim` (synthetic PT 1 kHz and load cell 333 Hz), `REC ON` for 29 s, `REC OFF`, `PFSFLUSH`. Then `daq_mtp_force` again.
* Result from `LIST`: file id 2, 173 pages, size 324548 bytes (323696 bytes of records). `ingested=323696 dropped=0`.
* Bytes written to the NAND: about 173 pages x 2048 B = 354 KB = 2.8 Mbit. That is 0.6 % of the 500 Mbit limit. The file stays on the NAND (no delete without a format).
* Total NAND writes of the MTP task: 2.8 Mbit (this run only). Before this run: 0.
