# MTP spike report (PAUSED: steps 1 to 4 done, step 5 not done)

Author: MTP spike agent. Date: 2026-11-11. Language: ASD-STE100. Labels: MTP-P-n.
Project: `tools/mtp_spike/` (standalone ESP-IDF, env `spike`). It has no PFS. It does not touch GPIO 10 to 13.

## MTP-P-1 Status in one table
| Step | State |
|---|---|
| 1 Check the board with INFO | Done. The answer was not from the DAQ firmware (see MTP-P-2). |
| 2 esp_tinyusb 3.0.0 builds on the S3 with IDF 6.1.0 and `override_path` | Done. Builds with no error. |
| 3 Fork of the MTP layer, read-only, fake VFS | Written and built. NOT tested on a host. |
| 4 Boot logic, grace time, CDC `DL` and `REBOOT` | Written and built. `DL` and `REBOOT` NOT tested. |
| 5 Test on the Linux host (lsusb, libmtp, speed) | NOT done. |
| 6 This report | Written at pause time. No measured data yet. |

## MTP-P-2 Board state
* The board on /dev/ttyACM0 did NOT run the DAQ firmware. It printed `@L:I:PFS-PIL READY 64 16 2048` once per second. `INFO` got no answer.
  This is probably a Paranoid-FS test firmware. So MODE1 state is unknown. That firmware is now overwritten (bootloader, partition table, app only).
  The `pfs_nor` partition (0x410000) and the NAND were not touched.
* The ORIGINAL firmware was not restored. The DAQ firmware was not on the board.
* NOW: the board runs the SPIKE firmware, env `spike` (no force flag). It boots in serial mode (USB Serial/JTAG, /dev/ttyACM0, 115200).
  It prints `SPIKE: serial mode, gpio2=1 ...` each second. So GPIO 2 (MODE2) reads HIGH: serial mode.
  The board is recoverable with the normal command (MTP-P-6). No BOOT button is needed in this state.
* Because GPIO 2 is high, files mode cannot start from the pin. Use env `spike_force` (MTP-P-6), or put the MODE2 switch low.

## MTP-P-3 What works (verified by build only)
* MTP-P-3.1 Stock component from the registry: `espressif/esp_tinyusb: "3.0.0"` in `src/idf_component.yml`. The component manager
  downloads it into `managed_components/`. It built the first time with PlatformIO platform 7.1.3 (IDF 6.1.0, GCC 15.2.0). No build error.
* MTP-P-3.2 `override_path` works under PlatformIO. `src/idf_component.yml` now says
  `override_path: "../fork/esp_tinyusb"` (path is relative to the manifest). `dependencies.lock` shows `path: fork/esp_tinyusb, type: local`.
  The firmware contains the fork strings ("refused (read-only)"). Do NOT put the fork in a folder named `components/`: IDF would then see a second
  component named `esp_tinyusb` next to `espressif__esp_tinyusb` (not tested, a risk of duplicate symbols).
* MTP-P-3.3 sdkconfig keys that work (in `tools/mtp_spike/sdkconfig.defaults`): `CONFIG_TINYUSB_CDC_ENABLED=y`, `CONFIG_TINYUSB_MTP_ENABLED=y`,
  `CONFIG_TINYUSB_MTP_MAX_STORAGES=1`, and the three `CONFIG_TINYUSB_DESC_*_STRING` keys. Defaults CDC_COUNT=1, MTP_EP_BUFSIZE=4096 are kept.
  The default descriptor already makes the composite (IAD + CDC + MTP, PID 0x4041 by the research; not yet read with lsusb).
* MTP-P-3.4 Size: flash 221 KB, RAM 29 KB (static).

## MTP-P-4 The fork (all in `tools/mtp_spike/fork/esp_tinyusb`, each change marked `FORK`)
1. `include/tinyusb_mtp.h`: storage config has new fields `total_bytes`, `free_bytes`. Size 0 means FATFS (old behaviour).
2. `include_private/mtp/tinyusb_mtp_types.h`: the storage record keeps both fields.
3. `mtp/tinyusb_mtp.c` (`register_storage`) and `mtp/tinyusb_mtp_operations.c` (`get_storage_info`): `esp_vfs_fat_info()` is called only when
   `total_bytes` is 0. A plain VFS works now. (These were the two FATFS checks.)
4. StorageInfo: AccessCapability = 0x0001 (read-only, no deletion). ObjectInfo: ProtectionStatus = 0x0001 (read-only).
5. `mtp/tinyusb_mtp_transport.c` (`mtp_dispatch`): SendObjectInfo, SendObject, DeleteObject, MoveObject and the four Android edit operations
   return 0x200E (Store_Read_Only). SetObjectPropValue returns 0x200F (Access_Denied).
6. `include/tusb_config.h`: those operations are removed from the DeviceInfo supported-operation list. So libmtp tools will refuse by themselves.
   To test the device-side refusal, use a raw PTP client, or add the operations back to the list in a test build.
* Not changed: GetPartialObject (0x101B) and GetPartialObject64 (0x95C1) stay in. The read path uses `fopen/fseek/fread` on the VFS.

## MTP-P-5 Spike firmware (`tools/mtp_spike/src`)
* `fake_vfs.c`: `esp_vfs_register("/daq")`, read-only. Root has folders `NAND` and `SD`. `NAND/F00001.BIN` = 3 MB, byte = (offset and 0xFF) xor 1.
  `NAND/F00002.BIN` = 100 KB, byte = (offset and 0xFF) xor 2. `SD` is empty. Bytes are made on the fly. Write calls fail with EROFS.
  The storage is reported with capacity 64 MB and 0 bytes free. ONE storage "DAQ" is registered; the host should see the two folders in its root.
* `main.c`: first call is the USB mux back to USB Serial/JTAG. GPIO 2 is read with pull-up. Low (or `-DSPIKE_FORCE_FILES=1`) means files mode.
  Files mode starts after a 10 s grace (`SPIKE_GRACE_MS`) with USB Serial/JTAG alive, so the board can be flashed. During grace it prints a countdown.
  Files mode: MTP + CDC (TinyUSB). CDC commands: `DL`, `REBOOT`, `INFO`, `LOG` (dump of a 6 KB log ring buffer, because the console is
  invisible in files mode), other lines are echoed.
* `DL` code: mux to USJ (`usb_serial_jtag_ll_phy_enable_external(false)`), write `RTC_CNTL_OPTION1_REG = RTC_CNTL_FORCE_DOWNLOAD_BOOT`, `esp_restart()`.
  `REBOOT`: the same without the register write. UNTESTED. Risk: the RTC mux may survive a reset, and FORCE_DOWNLOAD_BOOT may stay set after the
  esptool reset (see MTP-P-8). Test with the 10 s grace first.

## MTP-P-6 Commands (from the worktree root; use the penv pio, the system pio lacks `intelhex`)
```
cd tools/mtp_spike
~/.platformio/penv/bin/pio run                                   # build env spike (serial mode unless GPIO 2 low)
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0
~/.platformio/penv/bin/pio run -e spike_force                    # files mode always (after 10 s grace)
~/.platformio/penv/bin/pio run -e spike_force -t upload --upload-port /dev/ttyACM0
```
After a flash the port can change. `dmesg -w` shows it. Flash during the 10 s grace if the board is in files mode.
Versions: PlatformIO Core 6.2.0, platform espressif32 7.1.3, ESP-IDF 6.1.0, GCC 15.2.0, esptool 4.11.0, espressif/esp_tinyusb 3.0.0 (TinyUSB 0.21.0),
libmtp 1.1.23, libusb 1.0.30, python-pyserial 3.5. `jmtpfs` and `python-pyusb` are not installed. `mtp-detect`, `mtp-files`, `mtp-folders`,
`mtp-getfile` and `gio` are installed.

## MTP-P-7 Next 5 steps
1. Flash env `spike_force`. Watch that the 10 s grace works (serial prints the countdown), then that the device shows as 303a:4041 (`lsusb -v`).
2. Test `REBOOT` over the CDC port (pyserial). Check the board comes back in USB Serial/JTAG and can be flashed. THEN test `DL`
   (`esptool.py --before no_reset chip_id`, then reset). Stop and report if `DL` fails.
3. `mtp-detect`, `mtp-folders`, `mtp-files`: check ONE storage with folders NAND and SD, and the two files with the right sizes.
4. `mtp-getfile` for F00001.BIN. Compare with the pattern. Measure MB/s. Random range: GetPartialObject with a small raw libusb or libmtp C test
   (libmtp has `LIBMTP_GetPartialObject` in 1.1.x; write a 30-line C test). Raw PTP write test (SendObjectInfo) must give 0x200E.
5. Update this report with lsusb output, dmesg, timings and risks. Then restore a normal firmware (see MTP-P-2: the DAQ firmware was never on the board;
   flash it only with the MODE1 rule of AGENT_RULES.md).

## MTP-P-8 Open problems and risks
* The original board firmware was a PFS PIL firmware, not the DAQ firmware. The orchestrator should say which firmware the user wants back.
* GPIO 2 reads high on this board now: files mode needs `spike_force` or the switch.
* UNVERIFIED: `DL`/`REBOOT` recovery from TinyUSB mode (RTC mux and FORCE_DOWNLOAD_BOOT behaviour).
* UNVERIFIED: libmtp and GVfs accept the descriptors, ObjectInfo with ProtectionStatus 1, and the refused writes.
* The log ring uses `esp_log_set_vprintf`; in files mode the USJ console is dropped, not blocking (UNVERIFIED on target).
* The fork copy is 4.9 MB in git (mostly TinyUSB source). Keep one copy only.
* `git status` shows other agents' edits to `platformio.ini` and `src/` in this worktree. They are not part of the spike commits.

## MTP-P-9 Test run on the Linux host (second spike agent, 2026-11-11 01:2x)
Host: Arch Linux, kernel 7.2.11, xhci. `dmesg` needs root: use `journalctl -k --no-pager | grep -v PME`.

### MTP-P-9.1 Step 1: spike_force flash, grace, descriptors (PASS)
* `pio run -e spike_force -t upload --upload-port /dev/ttyACM0` worked from serial mode (USB Serial/JTAG 303a:1001, no BOOT button).
* The 10 s grace worked. USB Serial/JTAG printed `SPIKE: files mode in 5 s ...` down to 1, then the device became 303a:4041 "IREC DAQ".
* `lsusb -v -d 303a:`: Full Speed (12 Mbps), bDeviceClass 239/2/1 (IAD), 3 interfaces, wTotalLength 0x69, MaxPower 100 mA, self-reported bus powered.
  IAD (interfaces 0-1) = CDC ACM (iInterface "IREC DAQ serial", EP1 IN int 8 B, EP2 OUT/IN bulk 64 B).
  Interface 2 = class 6 / subclass 1 / protocol 1 (PTP), iInterface "IREC DAQ MTP", EP3 IN int 64 B, EP4 OUT/IN bulk 64 B.
  Linux binds `cdc_acm` (`/dev/ttyACM0`). Serial number string 2884854684E0.
* Limit: the ESP32-S3 USB OTG is Full Speed only, bulk packet 64 B. Real MTP read speed will be well below 1.2 MB/s (see MTP-P-9.4 once measured).
* CDC in files mode works: `INFO` -> `INFO: spike files mode, up 28 s, heap 353108`. Other lines are echoed.

### MTP-P-9.2 Step 2: the way back (REBOOT and DL) (found a bug, fixed, PASS after the fix)
* BUG (first firmware): CDC `REBOOT` and `DL` reset the chip, but the host never saw a disconnect. The D+ pull-up stays on across
  `esp_restart()`. The host kept the old stale device 303a:4041 (dead: no CDC answer) for the whole grace time, and for `DL` for ever.
  The chip was in ROM download mode, but invisible: `esptool --before no_reset` timed out. No BOOT button, no root, was needed to get out:
  `usbreset 303a:4041` (ACL gives the user write access to /dev/bus/usb; it prints "failed [No such device]" but the host
  re-enumerates the device as 303a:1001). Then `esptool --before no_reset chip_id` worked (ROM download mode, USB-Serial/JTAG). After the
  esptool hard reset the RTC flag was cleared and the board booted the app normally.
* FIX in `tools/mtp_spike/src/main.c` (files_mode loop): after the 300 ms CDC drain, call `tud_disconnect()` and wait 500 ms, then
  `mux_to_jtag()`, set the RTC flag (DL only) and `esp_restart()`.
* RESULT after the fix: `REBOOT` -> 1 s later 303a:1001 appears and stays for the 10 s grace, then 303a:4041 again. `DL` -> 303a:1001 within
  4 s (ROM download mode); `pio run -e spike_force -t upload --upload-port /dev/ttyACM0` flashed and verified with no human help.
* RULE for the real firmware: ALWAYS call `tud_disconnect()` + 500 ms before any restart from files mode. Fallback if the host shows a stale
  4041: `usbreset 303a:4041` (no root).
* Flash from files mode, full recipe: CDC `DL`; wait 4 s; `pio run -e <env> -t upload --upload-port /dev/ttyACM0`.
  (Scripts: pyserial, open /dev/ttyACM0 at 115200, write `DL\r\n`.)
(Checkpoint written after step 2 at 01:34. Step 3 failed: see MTP-P-9.3.)

### MTP-P-9.3 Step 3: libmtp (HARD FAILURE, board port LOST at 01:37)
* The board ran `spike_force` (with the `tud_disconnect` fix) in files mode, CDC alive (`INFO` answered, up 28 s).
* `mtp-detect` (libmtp 1.1.23, no udev entry for 303a:4041, only a warning "UNKNOWN") found the device (`Found 1 device(s): 303a:4041`),
  then FAILED at the first transaction: `PTP_ERROR_IO: failed to open session, trying again after resetting USB interface`,
  `LIBMTP PANIC: failed to open session on second attempt`. So the descriptors are accepted (class 6/1/1), but the first MTP bulk transfer
  (OpenSession on EP4 OUT / response on EP4 IN) does not complete. Cause NOT known (see hypotheses below).
* After libmtp's USB reset (kernel: `usb 1-1: reset full-speed USB device number 35`), the device re-enumerated (cdc_acm bound again),
  but then it was dead: CDC `INFO` got no answer, a second `mtp-detect` saw no raw device, `lsusb -v` returned only cached data.
  The firmware is hung or crashed in TinyUSB/MTP (no log: the console is invisible in files mode and `LOG` needs the CDC).
* `usbreset 303a:4041` then hung (process in D state). The kernel log shows: `device descriptor read/64, error -110`,
  `xhci_hcd 0000:c1:00.3: Timeout while waiting for setup device command`, `device not accepting address NN, error -62`,
  and at 01:39:05 `usb usb1-port1: unable to enumerate USB device`. The port is now empty for the host: NO /dev/ttyACM0, no 303a device in `lsusb`.
* No root is available (`sudo` needs a password). The sysfs `usb1-port1/disable` file is root-only. So the host port cannot be re-triggered by software.
  THE PORT IS LOST until a human unplugs and replugs the USB cable (or presses RESET on the board).
* RECOVERY after the replug: the board boots `spike_force`. It prints the 10 s grace on USB Serial/JTAG (303a:1001, /dev/ttyACM0).
  Flash IN THAT WINDOW with `cd tools/mtp_spike && ~/.platformio/penv/bin/pio run -e spike -t upload --upload-port /dev/ttyACM0`
  (env `spike` = serial mode when GPIO 2 is high; plain upload works). If the window is missed: CDC `DL` (files mode alive), else BOOT+RESET.
* Hypotheses for the OpenSession failure (for the next agent, with the board back):
  1. `CFG_TUD_MTP_EP_BUFSIZE`/`tud_mtp` buffer size 4096 is not a multiple of the 64 B Full Speed packet or a problem in how the class driver
     arms EP4 OUT. Check TinyUSB 0.21 `mtp_device.c` `mtpd_open` and the endpoint buffer config.
  2. No device-side trace: build with `CONFIG_TINYUSB_MTP_TRACE_WRITES=y` and read the ring buffer with CDC `LOG` BEFORE libmtp resets the device.
     Better: write a small raw libusb client (`/usr/include/libusb-1.0/libusb.h` is present, gcc is present, `python-pyusb` is NOT installed) that sends
     OpenSession (op 0x1002, tid 1, param 1) and prints each USB result. Never use `mtp-detect` first: its USB reset kills the device.
  3. The USB reset by libmtp (and `usbreset`) hangs the DWC2 stack: after a bus reset the TinyUSB device may need a `tud_connect` cycle.
     Look at the esp_tinyusb port for the S3 (`dcd_dwc2`, `usb_phy`), and at the line `device descriptor read/64, error -110`.
  4. Robustness idea: a hung files mode cannot be left from software. Add a task watchdog (`CONFIG_ESP_TASK_WDT_*` with the TinyUSB task
     subscribed, panic -> reboot) and an `esp_register_shutdown_handler()` that calls `tud_disconnect()`. A panic reboot also keeps the pull-up
     on (the host sees a stale device), so a hung unit still needs the cable replug. This is a real risk for the DAQ: keep the BOOT button reachable.
* Steps 3 (rest) and 4 NOT done: no MTP host test and no CDC-during-MTP test were possible.

## MTP-P-10 FIX agent run (2026-11-11 02:00 to 02:15)
Result in one line: patches P1, P2, P4 are built and the watchdog is PROVEN. Patch P3 (first version) broke the USB Serial/JTAG pad.
The board is now INVISIBLE to the host (port "not attached"). A human must put it in ROM download mode (hold BOOT, plug or reset). No MTP host test ran.
Commit f99bfc3 holds all firmware changes. Zero NAND writes (the spike has no NAND code).

### MTP-P-10.1 What was changed (all in tools/mtp_spike/)
* P1 `fork/esp_tinyusb/tinyusb/src/class/mtp/mtp_device.c`: `prepare_new_command()` has a busy check and a failure path (1c);
  DEVICE_RESET SETUP stage answers with `tud_control_status()` (1a); COMMAND phase re-arms OUT on a late IN completion or a ZLP,
  and a short or wrong-type packet sets ERROR (both bulk EPs halted, host recovers with 0x66) (1b). Built, NOT tested on the host.
* P2 `fork/esp_tinyusb/tinyusb_task.c`: the TinyUSB task subscribes to the task watchdog. Loop: `tud_task_ext(1000,false)` then `esp_task_wdt_reset()`.
* P4 `sdkconfig.defaults`: `ESP_TASK_WDT_PANIC=y`, `ESP_TASK_WDT_TIMEOUT_S=8`, `TINYUSB_DEBUG_LEVEL=0`, `TINYUSB_MTP_TRACE_WRITES=y`,
  and `ESP_SYSTEM_PANIC_SILENT_REBOOT=y` (added by me: a panic print to the dead USJ console could block the reset). TinyUSB task stack 6144.
  The generated files `sdkconfig.spike*` are git-ignored: delete them after you edit `sdkconfig.defaults`, or the edit has no effect.
* `src/main.c`: CDC commands `HANG` (the USB task spins for ever) and `ABORT` (`abort()`); shutdown handler `usb_shutdown` (tud_disconnect + 500 ms);
  P3 function (see 10.3). `host_cdc.py PORT CMD [seconds]` sends one CDC command.
* `host/ptp_probe.c`: raw libusb client, compiles with no warnings (`gcc -O2 -Wall -Wextra ptp_probe.c -lusb-1.0`). NOT run (no board). See 10.5.

### MTP-P-10.2 Watchdog proof (PASS, firmware v1 = P1+P2+P3v1+P4)
* 02:02:17 CDC `HANG` (USB task spins in `on_rx`). 02:02:25 the host log shows `USB disconnect` (8 s TWDT). 02:02:35 the device is back as 303a:4041
  (10 s grace, then files mode). CDC `INFO` after it says `up 34 s` (a real reboot). No human, no usbreset.
* The host saw the disconnect only because of P3 (a panic reboot keeps the old D+ pull-up, MTP-P-9.3).
* Flash path that worked: CDC `DL` (4 s) then `pio run -e spike_force -t upload --upload-port /dev/ttyACM0` (flash verified; the final
  "could not open port" line is only the esptool post-reset check, ignore it).

### MTP-P-10.3 Failure: P3 version 1 killed the USB Serial/JTAG pad (board lost at 02:03:33)
* P3 v1 ran at EVERY boot: `usb_wrap` and `usb_serial_jtag` pull override with D+/D- pull-down, 200 ms, then `disable_pull_override`.
* Symptom 1 (02:01:35 and 02:02:25): in the 10 s grace the host saw NO 303a:1001 (before P3 it did). After the grace, 4041 and CDC were fine.
* Symptom 2 (02:03:33): CDC `DL` -> `USB disconnect`, and then nothing for ever: `usb1-port1/state` = `not attached`, no 303a in `lsusb`, no kernel message.
  The chip is most likely in ROM download mode with no D+ pull-up.
* Cause (best explanation, UNVERIFIED): the USB Serial/JTAG registers survive a software reset (the device stays connected over `esp_restart()`).
  P3 left `conf0` with dp_pullup=0 and dp_pulldown=1. `disable_pull_override` does not restore the values. The ROM download mode then has no pull-up.
* Lesson: I saw symptom 1 and went on. I should have stopped there. DL must be re-tested on every new firmware BEFORE a second step.
* Fix in the source (commit f99bfc3, NOT flashed, NOT tested):
  (a) `usj_pads_restore()` writes the normal values (dp_pu=1, rest 0) with the override ON, then turns the override OFF. It runs at the end of P3
  and before every restart in the REBOOT and DL path.
  (b) P3 runs only after a crash reset (`ESP_RST_PANIC`, INT_WDT, TASK_WDT, WDT), not at power-on or after a software restart.
  So the path that was proven at 02:00 (DL from a clean boot) is unchanged. Build flag `-DUSB_P3_ALWAYS=1` forces P3 at every boot.

### MTP-P-10.4 How to recover the board (human needed) and what to do next
1. Hold the BOOT button (GPIO 0), plug the USB cable in (or press RESET while BOOT is held). `lsusb` must show 303a:1001. Release BOOT.
   (If a plain replug already gives 303a:1001 for 10 s, the board runs v1: flash in that window with the same command. Probably it does not.)
2. `cd tools/mtp_spike && ~/.platformio/penv/bin/pio run -e spike_force -t upload --upload-port /dev/ttyACM0` (builds the fixed v3).
3. Test in this order, one step at a time, stop at the first surprise:
   a. Power cycle: 303a:1001 in the grace? Then 4041 after 10 s? CDC `INFO`.
   b. CDC `DL`: does 303a:1001 come back in 4 s? If yes, flash the same build again through it (proves the way back).
   c. CDC `HANG`: 303a disconnect after ~8 s, back as 4041 after ~10 s (as in 10.2). Then 1001 visible in the grace? Then CDC `DL` again (the pads are
      restored by `usj_pads_restore()`). If any of b or c fails, the board needs BOOT again: do not go on, report.
   d. Only then the MTP host test: `gcc -O2 -Wall -o $SCRATCH/ptp_probe tools/mtp_spike/host/ptp_probe.c -lusb-1.0`, then stage by stage:
      `ptp_probe -c /dev/ttyACM0 -s 1`, `-s 2`, ... `-s 7`, then `-r` (DEVICE_RESET). Each transfer has a 3 s timeout. If the board stops
      answering, the watchdog resets it in 8 s (CDC `INFO` shows the uptime). Last step: `mtp-detect` once, then `mtp-files`, `mtp-folders`.

### MTP-P-10.5 ptp_probe design (for the next agent)
Interface 2, EP 0x04 OUT, 0x84 IN. No `libusb_reset_device`, no `set_configuration`, no `clear_halt`. Stages 1-7 as in the file header:
GET_DEVICE_STATUS, OpenSession, GetDeviceInfo, GetStorageIDs, GetStorageInfo, GetObjectHandles (root 0xFFFFFFFF, then NAND), GetObjectInfo,
GetObject F00002 (100 KB), GetObject F00001 (3 MB, prints KB/s, compares with the pattern `(offset & 0xFF) ^ file number`), 20 GetPartialObject
reads at fixed-seed random offsets (also offset 0, the last 100 bytes, an unaligned range and a read beyond EOF), SendObjectInfo and DeleteObject
(must be refused with 0x200E or 0x200F). `-c PORT` prints CDC `INFO` after each phase (the firmware task is alive or not) and the `LOG` ring on a failure.
Exit code 0 = PASS. Known weak spots: the SendObjectInfo refusal path (the code first waits 1 s for an immediate response, then sends the data phase);
a refusal after a data phase may trigger the P1b ERROR stall: then use `-r` to test the recovery.

### MTP-P-10.6 State of the board at the end of this run
Firmware on the flash: v1 (commit-equivalent of P1+P2+P3v1+P4, built before f99bfc3 changes to P3). It is the version with the pad bug, so the board
needs BOOT-button recovery (10.4). Hardware tests after that: none done by this agent. Zero NAND writes.

## MTP-P-11 Static review of the v3 USB Serial/JTAG pad safety (no hardware, 2026-11-11 ~03:00)
Verdict: SAFE for every path that v3 (default build) can take, with two items marked UNVERIFIED (11.6). No board access was used. Zero NAND writes.
Sources: IDF 6.1 in ~/.platformio/packages/framework-espidf/components (paths below are relative to it).

### MTP-P-11.1 Register facts (from the IDF sources)
* USJ pad control is `USB_SERIAL_JTAG.conf0` (`soc/esp32s3/register/soc/usb_serial_jtag_reg.h`): reset defaults `PAD_PULL_OVERRIDE=0`, `DP_PULLUP=1`,
  `DP_PULLDOWN=0`, `DM_PULLUP=0`, `DM_PULLDOWN=0`, `USB_PAD_ENABLE=1`, `PHY_SEL=0`. (The USB_WRAP defaults differ: all pull bits 0.)
* `usb_serial_jtag_ll_phy_disable_pull_override()` only clears the override bit. It does NOT restore the four pull values
  (`esp_hal_usb/esp32s3/include/hal/usb_serial_jtag_ll.h:264-279`). This is how v1 left `dp_pullup=0, dp_pulldown=1, dm_pulldown=1` in conf0.
* The PHY mux is in the RTC domain: `RTCCNTL.usb_conf.sw_hw_usb_phy_sel` and `sw_usb_phy_sel` (`usb_serial_jtag_ll.h:209-220`).
  `sw_hw=1, sw_usb_phy_sel=0` = the internal PHY belongs to the USJ.
* IDF itself gives the PHY back to the USJ with `phy_enable_external(false)` plus `phy_enable_pad(true)`
  (`esp_hal_usb/usb_serial_jtag_hal.c:19-27`, `esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:207-208`).
* Reset classes (S3): panic, task WDT, interrupt WDT and `esp_restart()` end in `esp_rom_software_reset_cpu()` (`esp_system/port/soc/esp32s3/system_internal.c:esp_restart_noos`,
  `esp_system/port/panic_handler.c:panic_restart`). A CPU reset keeps the clocks and the registers of USJ and USB_WRAP: `esp_system/port/soc/esp32s3/clk.c:236-245`
  keeps the clock state for CPU0_SW / CPU0_MWDT / CPU0_RTC_WDT, and the USJ block is skipped by the reset list (clk.c:329).
  This is the reason why the v1 pad state survived `esp_restart()`. Brown-out ends in `esp_rom_software_reset_system()` (`esp_hw_support/power_supply/brownout.c:80`), a core reset:
  the digital peripherals restart, the RTC registers stay (comment in `esp_system/port/esp_system_chip.c:46`). A power-on resets everything.
* The fork's `dcd_disconnect()` (`tools/mtp_spike/fork/esp_tinyusb/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c:591-607`) writes the USB_WRAP pull override
  (override=1, D+/D- pull-down) and DWC2 DCTL.SDIS. `tud_disconnect()` is therefore NOT a pure DWC2 action. It leaves USB_WRAP dirty. This state was present in the
  REBOOT/DL path that worked on hardware (MTP-P-9.1: DL gave 303a:1001 in 4 s). It is harmless because the PHY is on the USJ then, and `tinyusb_driver_install()`
  resets USB_WRAP (`esp_hal_usb/usb_wrap_hal.c:_usb_wrap_hal_init`) before the next use.
* `tud_disconnect()` before `tinyusb_driver_install()` is NOT safe (`_usbd_rhport` is RHPORT_INVALID, `usbd.c:414,516`). v3 never calls it there.

### MTP-P-11.2 Invariant of v3 (default build)
v3 writes to `USB_SERIAL_JTAG.conf0` only: `phy_sel=0` (reset default), `usb_pad_enable=1` (reset default), and the four pull fields plus the override bit,
but ONLY when `usj_pads_restore()` finds a value that differs from the reset default, and then it writes the reset default. So conf0 is at the reset default after every
boot step of v3, whatever path ran before. The default build never writes USB_WRAP, DWC2 or `RTCCNTL.usb_conf` at boot, except `mux_to_jtag()`
(RTC `sw_hw=1, sw_usb_phy_sel=0`; this line existed in the build that was proven at 01:3x, commit a6004ca).

### MTP-P-11.3 Proof table (state at the first 10 s of the next boot, the grace window)
| Path | What runs | conf0 at app_main | RTC mux | Grace window | Source / status |
|---|---|---|---|---|---|
| Power-up | chip reset, all registers at reset default; `usj_pads_restore()` reads conf0: clean, writes nothing; `mux_to_jtag()` | default | USJ (sw) | OK, USJ drives D+ pull-up | reg.h defaults. Same as the proven build. |
| `esp_restart()` (REBOOT) | `tud_disconnect()` (USB_WRAP dirty, DWC2 SDIS), 500 ms, `mux_to_jtag()`, restore (no-op), `esp_restart()`, CPU reset | default (v3 never changed it) | USJ | OK. USB_WRAP dirty but unused | Same writes as a6004ca (proven) + a no-op. The `usb_shutdown` handler of v1/v2 is removed (it was unproven and redundant). |
| `DL` | same as REBOOT, then `RTC_CNTL_OPTION1 = FORCE_DOWNLOAD_BOOT` before the restart. ROM download mode uses the USJ | default | USJ | ROM: 303a:1001, esptool works | Same as a6004ca (proven: 4 s). v1 failed here only because conf0 was dirty. |
| Panic / `abort()` / TWDT / INT WDT | CPU reset, no cleanup code runs. Mux and DWC2 stay as in files mode | default (v3 never changed it) | still OTG until `mux_to_jtag()` runs at the start of app_main, then USJ | Pad works (USJ owns it, pull-up on). The HOST may keep a stale device 4041 (D+ never dropped), so 1001 may not show: replug (power cycle) gives a clean boot | clk.c:236-245 (state kept). With `-DSPIKE_P3=1` the disconnect is tried (11.5). |
| RTC WDT (`esp_restart_noos` arms it 1 s) | CPU or system reset, same as above | default | as above | as above | `system_internal.c:esp_restart_noos` |
| Brown-out | core reset (`brownout.c:80`): digital peripherals restart, RTC registers stay | default (digital reset) or default (v3 never changed it) | RTC bits may stay OTG, `mux_to_jtag()` fixes them in app_main | Pad works. USB power is probably gone anyway | UNVERIFIED which registers the core reset clears. Both cases give default conf0. |
| Stale v1 state (board flashed before power cycle) | `usj_pads_restore()` repairs conf0 | dirty then repaired | USJ | OK after ~0.3 to 0.5 s of IDF start. ROM and the bootloader run with the dirty pad (they do not use USB) | A power-on resets conf0 anyway. |
No v3 path writes a non-default pull value into conf0. Therefore v3 cannot kill the USJ pad by the v1 mechanism.

### MTP-P-11.4 Code changes (tools/mtp_spike/src/main.c, platformio.ini)
* `usj_pads_restore()`: now reads conf0 first. Clock off or all fields at default: it writes nothing. Else it writes the reset default and sets `usb_pad_enable=1`. It returns true if it repaired something.
* It runs FIRST in `app_main()`, before the reset reason, P3 and `mux_to_jtag()`. It also runs before restart in the REBOOT and DL path (no-op there).
  Order check: the ROM and the second stage bootloader do not write conf0 (`bootloader_support/src/bootloader_console.c`: USJ console = ROM output only), so there is nothing to do before app_main. A dirty state lives ~0.5 s longer than necessary only.
* P3 v1 (`usb_force_disconnect`, pull override on USJ and USB_WRAP at every boot) is DELETED. `USB_P3_ALWAYS` is deleted. New flag `SPIKE_P3`, default 0 (env `spike_force_p3` sets it to 1).
  With `SPIKE_P3=1` and a crash reset only, `crash_disconnect()` replays the proven `dcd_disconnect()` writes (USB_WRAP override + DWC2 DCTL.SDIS, 500 ms), and only when the OTG clock is on
  and the RTC mux still maps the PHY to the OTG. It never writes conf0 (the register that killed the pad).
* `usb_shutdown` handler and its `esp_register_shutdown_handler()` call are removed (unproven, and the REBOOT/DL path already does the same before it calls `esp_restart()`).
* The boot log (CDC `LOG`) shows the reset reason, whether the pads were dirty, and the P3 flag.
* Not changed: the REBOOT and DL path (identical to the proven a6004ca path plus a no-op), P1, P2, P4, HANG, ABORT.
* Builds: `pio run -e spike_force`, `-e spike`, `-e spike_force_p3` all SUCCESS (penv pio). Not flashed.

### MTP-P-11.5 Is P3 needed? Why it is off
It is not needed for safety. Without P3, a crash (WDT) leaves the host with a stale device (D+ never dropped), so the host will not show 1001 or 4041 again until a replug.
The pad is fine, the chip runs, and a plain replug (power-on) gives a clean boot and a 10 s grace window. P3 only saves that replug in the HANG test. Its safety rests on a replica of
writes that already ran on hardware, but running it at boot after a crash is new, so it is UNVERIFIED and off.

### MTP-P-11.6 Remaining uncertainty
1. UNVERIFIED: why v1 killed the pad. Best explanation: stale conf0 values matter even with `pad_pull_override=0`. v3 never leaves them, so this does not matter for v3. If the pad still dies, the cause is something else: STOP, do not continue.
2. UNVERIFIED: brown-out reset scope (which registers a core reset clears). Both outcomes are covered (conf0 default either way).
3. UNVERIFIED: `SPIKE_P3=1` (does the RTC mux still say OTG at app_main after a crash, does the host see the disconnect). Failure is benign (no write happens, or the host stays stale).
4. UNVERIFIED: a leftover USB_WRAP override does not matter while the PHY is on the USJ. It held in the proven REBOOT/DL path (a6004ca), but not for the very first boot after v1.
5. After DL and a manual esptool flash the next boot depends on `FORCE_DOWNLOAD_BOOT` being cleared by esptool or the power cycle. This worked at 01:3x. If the board boots into download mode again, a power cycle clears it.
6. The files-mode path itself (MTP, TinyUSB hang) is not in scope here: a hang is recovered by the 8 s TWDT, then a replug (P3 off).

### MTP-P-11.7 Recovery and test order (human, then the next agent). Stop at the first surprise. Never run DL, REBOOT or a flash after step 3 until steps 1 to 3 pass.
Human:
1. Hold BOOT, plug the USB cable in. `lsusb` must show `303a:1001`. (Full power loss needed: the v1 pad state is cleared by the power-on. Unplug first, wait 3 s.) Release BOOT after the flash command starts.
2. `cd tools/mtp_spike && ~/.platformio/penv/bin/pio run -e spike_force -t upload --upload-port /dev/ttyACM0`. Then unplug, wait 3 s, plug in WITHOUT BOOT. Do not use `usbreset`.
Agent (after the human says the board is plugged in, no BOOT):
3. Power-cycle proof: within 10 s `lsusb` shows `303a:1001` (grace). After 10 s `303a:4041`. CDC `INFO` answers. Then CDC `LOG`: the line "USJ pads were clean" (a "DIRTY" line is also fine for the first boot, but report it).
   If 1001 is missing in the grace window: STOP. Ask the human for BOOT + replug.
4. `DL`: expect `303a:1001` in about 4 s. Flash the same `spike_force` again through it (proves the way back). Then power cycle once (step 3 again).
5. `REBOOT`: expect disconnect, then 1001 for 10 s, then 4041. Check `LOG` ("reset reason 11" or similar = software, "pads clean").
6. `HANG` (TWDT test, P3 off): the board resets after 8 s. The host may keep a stale 4041. That is expected. Ask the human for a replug (or skip this step; the WDT proof of MTP-P-10.2 stands). Only after steps 3 to 5 passed, and only if the human is at the board, flash `-e spike_force_p3` through `DL` and repeat `HANG`: the host should see a disconnect and 4041 again. If not: go back to `spike_force`.
7. MTP host tests, with `ptp_probe` only (see MTP-P-10.4 item d). Never `mtp-detect` first, never `usbreset`.

## MTP-P-12 ptp_probe against the real firmware
`tools/mtp_spike/host/ptp_probe.c` now tests the real `daq_mtp` storage (NAND_F<hex>.BIN in folder NAND, uSD_F<hex>.BIN in folder SD, read-only). The old fake-pattern mode stays behind `--fake`.
Build: `gcc -O2 -Wall -Wextra -o ptp_probe ptp_probe.c $(pkg-config --cflags --libs libusb-1.0)`. Not run on hardware yet.
Options: `-c PORT`, `-s STAGE` (default 6), `-m MB` (default 8), `-r`, `--fake`.
* Stage 3: lists each root folder and its files with sizes. An empty folder prints "folder empty" and counts as OK.
* Stage 4: reads the first NAND file (whole, or the first MB megabytes) two times. It prints CRC32 and speed. The two CRCs and the bytes must be equal. No file means skip (OK).
* Stage 5: 20 GetPartialObject reads with lengths 1, 7, 8, 1876, 1877, 4096, 65536 and offsets near page boundaries (fixed pseudo-random sequence). Each result must equal the bytes held from stage 4.
* Stage 6: SendObjectInfo, DeleteObject and FormatStore must each give a PTP error, never 0x2001. Then GetStorageIDs must still answer.
* Every USB transfer has a timeout of 4 s or less (2 s for the refusal response). The exit code is 0 only if all checks pass.
