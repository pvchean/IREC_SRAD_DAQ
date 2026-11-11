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
