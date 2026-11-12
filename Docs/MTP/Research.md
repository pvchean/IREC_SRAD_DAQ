# MTP research: how to make a USB MTP responder on the ESP32-S3 DAQ

Date: 2026-11-11. Author: MTP research agent. Scope: research only. No firmware was written. No board was flashed.
Rule: a detail marked UNVERIFIED is a guess or was not tested. The files read for this document are in `/tmp` and are not kept;
the links and version numbers below let a new agent fetch them again.

## MTP-R-1 Summary and recommendation

* MTP-R-1.1 Upstream TinyUSB has an MTP class driver. It is in release 0.19.0 and later (0.21.0 is the newest release).
* MTP-R-1.2 Espressif carries it. The component `espressif/esp_tinyusb` 3.0.0 (2026-10-20) bundles TinyUSB 0.21.0 and has a
  ready MTP layer, a CDC-ACM layer, and default composite descriptors (CDC + MTP). It supports GetPartialObject (0x101B) and
  GetPartialObject64 (0x95C1). It is read-write today and it needs a FATFS mount for each storage (see MTP-R-3.3).
* MTP-R-1.3 The PFS (Paranoid-FS) volume is not a VFS and not FATFS. So the stock Espressif MTP layer cannot show it.
  The fix is small: a local copy of the MTP layer with two changed spots, plus a read-only VFS shim for PFS.
* MTP-R-1.4 ESP32-S3 has one internal USB PHY. USB Serial/JTAG and USB OTG share it. The code decides at boot (GPIO 2).
  Serial mode stays as it is. Files mode uses TinyUSB (MTP + CDC-ACM). This is safe (see MTP-R-5).
* MTP-R-1.5 Descriptor that works on Windows and Linux: IAD + CDC-ACM (interfaces 0, 1) + MTP interface 2 with class
  0x06, subclass 0x01, protocol 0x01, three endpoints, and an interface string that contains "MTP".
* MTP-R-1.6 macOS has no built-in MTP. OpenMTP is the best third-party tool. Image Capture (ptpcamerad) can block it.
  If native macOS access is a hard need, use option C (read-only USB mass storage with a synthetic FAT) - see MTP-R-9.
* MTP-R-1.7 Ranking: A (esp_tinyusb 3.0.0 + local fork of its MTP layer) first. B (own MTP code on the upstream class driver)
  second. C (read-only MSC with synthetic FAT) is the fallback for macOS. Details are in MTP-R-8 and MTP-R-9.

## MTP-R-2 Upstream TinyUSB MTP class driver (question 1)

Sources:
* Repository: <https://github.com/hathach/tinyusb>
* Driver: <https://github.com/hathach/tinyusb/tree/master/src/class/mtp> (`mtp.h` 897 lines, `mtp_device.h` 162, `mtp_device.c` 817)
* Example: <https://github.com/hathach/tinyusb/tree/master/examples/device/mtp> (`mtp_fs_example.c` 750 lines,
  `usb_descriptors.c` 248, `tusb_config.h` 145, `main.c` 190)
* Releases: 0.18.0 has no `src/class/mtp` (HTTP 404 on the raw file). 0.19.0 (2025-10-06) has it. 0.20.0 (2025-11-20),
  0.21.0 (2026-06-30). The driver still changes on master (for example commit 91cdc7b of 2026-11-04, and
  3c43dbc of 2026-10-24 for data OUT phases with ContainerLength 0xFFFFFFFF). Those changes concern uploads, not reads.

Facts from the source (master, read on 2026-11-11):
* MTP-R-2.1 Config macros (in `tusb_config.h`): `CFG_TUD_MTP` (1 = on), `CFG_TUD_MTP_EP_BUFSIZE` (a multiple of the bulk packet
  size, at most 65535), `CFG_TUD_MTP_EP_CONTROL_BUFSIZE` (16 is enough), and the DeviceInfo lists
  `CFG_TUD_MTP_DEVICEINFO_EXTENSIONS`, `_SUPPORTED_OPERATIONS`, `_SUPPORTED_EVENTS`, `_SUPPORTED_DEVICE_PROPERTIES`,
  `_CAPTURE_FORMATS`, `_PLAYBACK_FORMATS`. Optional: `CFG_TUD_MTP_LOG_LEVEL`.
* MTP-R-2.2 Only one MTP instance is possible (the driver has one static `_mtpd_itf`).
* MTP-R-2.3 Descriptor macro: `TUD_MTP_DESCRIPTOR(itf, stridx, ep_evt, ep_evt_size, ep_evt_interval, ep_out, ep_in, ep_size)`.
  It makes interface class 6 (Image), subclass 1, protocol 1 (PIMA 15740), plus an interrupt IN endpoint and a bulk OUT and
  bulk IN pair. `mtpd_open()` refuses any other class triple (`TU_VERIFY` on class, subclass, protocol).
* MTP-R-2.4 Callbacks the application writes: `tud_mtp_command_received_cb`, `tud_mtp_data_xfer_cb`, `tud_mtp_data_complete_cb`,
  `tud_mtp_response_complete_cb`, and the control callbacks `tud_mtp_request_cancel_cb`, `..._device_reset_cb`,
  `..._get_device_status_cb`, `..._get_extended_event_cb`, `..._vendor_cb`. API calls: `tud_mtp_data_send`,
  `tud_mtp_data_receive`, `tud_mtp_response_send`, `tud_mtp_event_send`, `tud_mtp_mounted`.
* MTP-R-2.5 The driver does the container framing, the bulk phases, ZLP, cancel, and device reset. It does not parse
  operations. The application decodes each command (op code in `command_container->header.code`, parameters in `params[]`).
* MTP-R-2.6 Read-only: there is no read-only switch. The application is read-only when it leaves out the write operations
  (SendObjectInfo 0x100C, SendObject 0x100D, DeleteObject 0x100B) from the supported list and answers
  `MTP_RESP_OPERATION_NOT_SUPPORTED` or `ACCESS_DENIED`. The StorageInfo dataset has an AccessCapability field.
  Value 0x0001 means "read-only, no deletion". UNVERIFIED: how each Windows version shows this.
* MTP-R-2.7 Random reads: yes. `MTP_OP_GET_PARTIAL_OBJECT` (0x101B) is in `mtp.h`. The example handles it in
  `fs_get_partial_object()`: offset and max bytes are `params[1]` and `params[2]`. Large replies go in pieces: the first
  `CFG_TUD_MTP_EP_BUFSIZE` minus 12 bytes in the command callback, the rest in `tud_mtp_data_xfer_cb` with the offset from
  `total_xferred_bytes`. GetPartialObject64 (0x95C1, an Android extension) is not in the upstream enum. The application
  can still receive it as a raw op code. Espressif's layer does this (MTP-R-3.3). Offsets are 32-bit in 0x101B; the
  NAND (512 Mbit = 64 MB) fits in 32 bits.
* MTP-R-2.8 Storage IDs and folders: the example uses one storage, ID `0x00010001`. GetObjectHandles gets
  (storage, format, parent). Parent 0xFFFFFFFF means the root. A folder is an object with format
  `MTP_OBJ_FORMAT_ASSOCIATION` (0x3001) and association type 1. Handles are 32-bit numbers chosen by the application.
  The example keeps a flat table `fs_objects[]` with a `parent` field.
* MTP-R-2.9 The example supported-operation list has no object-property operations (GetObjectPropsSupported 0x9801,
  GetObjectPropValue 0x9803, GetObjectPropList 0x9805). Windows and libmtp work without them (they fall back to
  GetObjectInfo). A third-party report says an STM32 USBX MTP, built from this example, works in Windows Explorer for the
  root folder: <https://stcommunity.st.com/t5/stm32-mcus-embedded-software/stm32-azurertos-usbx-mtp-hierarchical-folder-structure-not/td-p/895340>.
* MTP-R-2.10 The upstream example has HIL tests (`test/hil/pymtp.py`, `test/hil/mtp_raw.py`, PR #3956 of
  2026-10-01, "mtp: fix data phase completion and transaction recovery"). No Windows or macOS test report was found.

## MTP-R-3 Espressif components (question 2)

* MTP-R-3.1 IDF 6.1.0 (`~/.platformio/packages/framework-espidf`, `version.txt` = 6.1.0) has no TinyUSB. Its `components/`
  folder has only `esp_driver_usb_serial_jtag`, `esp_hal_usb`, and `esp_usb_cdc_rom_console` for USB. The USB PHY driver is
  `components/esp_hw_support/usb_phy/usb_phy.c`. So the app must add TinyUSB itself through the component manager.
  The project already uses the manager (`src/idf_component.yml`, `dependencies.lock`).
* MTP-R-3.2 Registry data (queried 2026-11-11, <https://components.espressif.com/components/espressif/esp_tinyusb> and
  <https://components.espressif.com/components/espressif/tinyusb>):
  | component | version | date | note |
  |---|---|---|---|
  | espressif/esp_tinyusb | 3.0.0 | 2026-10-20 | bundles TinyUSB 0.21.0 as a submodule folder `tinyusb/`; needs IDF >= 5.3; targets s2, s3, p4, h4, s31 |
  | espressif/esp_tinyusb | 2.4.0 | 2026-10-05 | depends on `espressif/tinyusb >= 0.17.0~2`; has MTP (added in 2.3.0, 2026-09-13) |
  | espressif/tinyusb | 0.21.0~4 | 2026-11-05 | plain TinyUSB 0.21.0; its CMake always compiles `src/class/mtp/mtp_device.c` |
  Changelog of 3.0.0: "Each release now ships TinyUSB as a git submodule with a fixed version ... Independent TinyUSB version
  selection is no longer supported." With 2.4.0, the TinyUSB version can be pinned separately.
* MTP-R-3.3 Espressif MTP layer, in `esp_tinyusb` 3.0.0 (folder `mtp/`, 9 files, 4114 lines; header `include/tinyusb_mtp.h`
  103 lines). Source: <https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb/mtp>. Origin: esp-usb
  PR/issue #531 "feat(esp_tinyusb): add TinyUSB MTP storage support" (2026-09-03).
  * It uses the upstream class driver (`mtp_device.c`) and writes all callbacks itself (`tinyusb_mtp_transport.c`).
  * API: `tinyusb_mtp_install_driver(&cfg)`, `tinyusb_mtp_register_storage({base_path, display_name, removable}, &h)`,
    then `tinyusb_driver_install()`. Up to 3 storages on the S3.
  * It lists folders and files with POSIX `opendir/stat/fopen/fseek/fread` on a mounted VFS path. It caches objects
    (up to 1024 on the S3). The storage is reported as read-write (`MTP_ACCESS_CAPABILITY_READ_WRITE` in
    `tinyusb_mtp_operations.c:105`). It supports Send/Delete/Move/rename and the Android edit extensions
    (0x95C1 to 0x95C5). It supports the object-property operations (GetObjectPropsSupported, GetObjectPropDesc, GetObjectPropValue,
    SetObjectPropValue, GetObjectPropList, GetObjectPropReferences) for GVfs and Windows.
  * Read path: `tinyusb_mtp_read.c` opens the file, `fseek` to the offset, and `fread` in each USB callback. It handles
    0x101B and 0x95C1 (`mtp_op_get_partial_object`, `mtp_op_get_partial_object64`).
  * The FATFS limit: `tinyusb_mtp_register_storage()` calls `esp_vfs_fat_info(base_path, ...)` and fails with
    `ESP_ERR_NOT_FOUND` if the path is not a FATFS mount (`tinyusb_mtp.c:233`). `tinyusb_mtp_operations.c:94` calls it again
    for StorageInfo. These are the only two FATFS-specific spots found.
  * Kconfig (menu "Media Transfer Protocol (MTP)"): `CONFIG_TINYUSB_MTP_ENABLED` (default n),
    `CONFIG_TINYUSB_MTP_EP_BUFSIZE` (4096, range 4096 to 8192 on S3), `CONFIG_TINYUSB_MTP_MAX_STORAGES` (3, range 1 to 3),
    `CONFIG_TINYUSB_MTP_MAX_OBJECTS` (512, range 32 to 1024), `CONFIG_TINYUSB_MTP_TRACE_WRITES`,
    `CONFIG_TINYUSB_DESC_MTP_STRING` (default "Espressif MTP Device"; it contains "MTP", which libmtp needs, see MTP-R-6.5).
  * Test app: `device/esp_tinyusb/test_apps/mtp_storage` (pytest with Linux GVfs; opt-in speed and many-file tests).
    No Windows or macOS test was found. UNVERIFIED on those hosts.
* MTP-R-3.4 CMake: when `CONFIG_TINYUSB_MTP_ENABLED` is set, the component compiles `tinyusb/src/class/mtp/mtp_device.c` and the
  nine `mtp/*.c` files. When `CONFIG_TINYUSB_CDC_ENABLED` is set it compiles the CDC files. The class macros
  (`CFG_TUD_MTP`, `CFG_TUD_CDC`) come from `include/tusb_config.h`, which reads the Kconfig values. The app cannot change
  them in its own code; it changes sdkconfig.
* MTP-R-3.5 Custom descriptors: `tinyusb_config_t.descriptor` has pointers `device`, `qualifier`, `string`, `string_count`,
  `full_speed_config`, `high_speed_config`. NULL means "use the default". The default config descriptor (file
  `usb_descriptors.c`) already builds IAD + CDC + MTP in one configuration when both classes are enabled
  (`TUD_CDC_DESCRIPTOR` then `TUD_MTP_DESCRIPTOR`, bulk size 64 at full speed). The default PID is `0x4000 | class bits`,
  with bit 0 = CDC and bit 6 = MTP, so CDC + MTP gives 0x4041 with VID 0x303A.
* MTP-R-3.6 How to pull code into a PlatformIO ESP-IDF project (UNVERIFIED for this exact project; the manager itself works
  because `pfs` and `littlefs` already come this way):
  1. Stock: add to `src/idf_component.yml`: `espressif/esp_tinyusb: "~3.0.0"`. The component manager resolves it from the
     registry at build time and writes `dependencies.lock` and `managed_components/espressif__esp_tinyusb`.
  2. Fork: copy `managed_components/espressif__esp_tinyusb` to `components/esp_tinyusb` (outside `managed_components/`,
     which is git-ignored or rebuilt) and point to it with
     `esp_tinyusb: { override_path: "../components/esp_tinyusb" }` (path is relative to the manifest). The component manager
     documents `override_path`. UNVERIFIED: that PlatformIO picks up a root `components/` folder; `override_path` avoids the question.
  3. Newer TinyUSB than the bundled one: with 3.0.0 not possible without editing the fork's `tinyusb/` folder. With
     `esp_tinyusb 2.4.0` plus `espressif/tinyusb` you can set `espressif/tinyusb: { git: https://github.com/hathach/tinyusb.git, version: <tag or sha> }`
     UNVERIFIED (the git source needs an `idf_component.yml` and CMake; the upstream repo has `idf_component.yml`
     and `CMakeLists.txt`). The bundled 0.21.0 is enough for read-only MTP, so this is not needed.

## MTP-R-4 ESP32-S3 USB hardware (question 3, part 1)

* MTP-R-4.1 One PHY. The S3 has one internal full-speed PHY on GPIO 19 (D-) and GPIO 20 (D+). A mux sends it to USB
  Serial/JTAG (USJ) or to the USB OTG controller (DWC2). Evidence, IDF 6.1.0:
  `components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h` lines 55 to 66: `usb_wrap_ll_phy_enable_external()` sets
  `RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1` and `sw_usb_phy_sel = !enable`. The comment says: 0 = internal PHY is mapped to
  the USJ; 1 = internal PHY is mapped to the USB Wrap (OTG). `soc_caps.h` for S3: `SOC_USB_FSLS_PHY_NUM 1`,
  `SOC_USB_OTG_PERIPH_NUM 1`.
* MTP-R-4.2 `esp_tinyusb` (`tinyusb.c`) calls `usb_new_phy()` with `.controller = USB_PHY_CTRL_OTG`, `.target = USB_PHY_TARGET_INT`,
  full speed. `usb_new_phy()` (`esp_hw_support/usb_phy/usb_phy.c`) runs `usb_wrap_hal_init()` (which enables automatic PHY
  control) and then `usb_wrap_hal_phy_set_external(false)` or equivalent. After this call the mux points to OTG. The USJ
  loses the pads. There is no software call that gives the pads back to the USJ while the app runs; a reset does it
  (the default after reset is USJ, because the ROM and the 2nd-stage bootloader use the USJ). UNVERIFIED: the exact
  register state after `esp_tinyusb` uninstall.
* MTP-R-4.3 Switch at boot, decided by GPIO 2 (MODE2, `board.h`: `MODE2_PIN 2`; `io.c` already reads it in `io_init()` and sets
  `io_usb_files_mode()`): safe in principle. Reasons: (a) the chip always starts on the USJ (ROM boot, bootloader, early app);
  (b) the app switches the mux once, early in `app_main()`, before any USB driver starts; (c) the host sees a disconnect of
  `303a:1001` and then a new device, so `/dev/ttyACM0` goes away and a new `ttyACM` appears. UNVERIFIED on this board: the
  re-enumeration time and the effect on a running `pio device monitor`.
* MTP-R-4.4 Console: `sdkconfig.defaults` has `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`. In files mode the console (ESP_LOG, printf)
  still writes to the USJ. The VFS driver `esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c` drops the bytes when
  `usb_serial_jtag_is_connected()` is false (lines 185, 233, 360; the connection monitor looks at SOF). So it does not block,
  UNVERIFIED on target. Safer: set the log level to NONE in files mode, or route the console to the CDC port
  (`tinyusb_console_init()` in `tinyusb_console.c`, needs `CONFIG_TINYUSB_CDC_ENABLED` and VFS support).
* MTP-R-4.5 Endpoints: the S3 DWC2 port has 7 endpoints, 5 IN (`dwc2_esp32.h`: `ep_count = 7, ep_in_count = 5,
  otg_dfifo_depth = 256` words). CDC needs 2 IN (notification, data) + 1 OUT. MTP needs 2 IN (event, bulk) + 1 OUT.
  Total 4 IN + EP0. It fits, with one IN endpoint spare. UNVERIFIED: the FIFO split (256 words = 1 KB total) at 64-byte
  packets; TinyUSB sets it at start-up, and the Espressif default CDC + MTP case is the test.
* MTP-R-4.6 WiFi: the S3 has `SOC_WIFI_PHY_NEEDS_USB_WORKAROUND` in `soc_caps.h`. The live-telemetry firmware uses WiFi. IDF
  handles this internally. UNVERIFIED: files mode with WiFi on. Files mode should keep WiFi off (the user wants file access, not live data).
* MTP-R-4.7 Memory: the USB DMA buffers must be in internal RAM. The project has `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`, so plain
  `malloc` does not use PSRAM. Good. Keep it that way for all MTP buffers.
* MTP-R-4.8 FS limit: USB is full speed (12 Mbit/s). Bulk packets are 64 bytes. Expect about 0.5 to 1 MB/s at best. The data
  size of the DAQ (500 B/s per spec rate) makes this fine.

## MTP-R-5 What must change in the current firmware (question 3, part 2)

Current use of the USJ (files in `src/`):
* `cmd.c`: includes `driver/usb_serial_jtag.h` and `usb_serial_jtag_vfs.h`. It writes replies with
  `usb_serial_jtag_write_bytes()` (lines 97, 150, 330, 368), reads commands with `usb_serial_jtag_read_bytes()` (line 644),
  and `cmd_start()` installs the USJ driver and the VFS (lines 655 to 659), then starts `cmd_task`.
* `stream.c`: line 101 calls `usb_serial_jtag_write_bytes()` for the live number lines.
* `main.c`: `app_main()` calls `io_init()` (reads MODE1, MODE2, ADDR pins) before PFS starts.
* `CMakeLists.txt` of `src/`: `PRIV_REQUIRES` has `esp_driver_usb_serial_jtag`.

Changes for files mode (design, not yet built):
* MTP-R-5.1 One small transport layer, for example `usb_io.c/.h`, with `usb_io_write(buf, len, timeout)` and
  `usb_io_read(&c, timeout)`. Backend 1 (serial mode, default): the USJ calls, as now. Backend 2 (files mode): the
  TinyUSB CDC-ACM port (`tinyusb_cdcacm_*` or the console VFS). `cmd.c` and `stream.c` call only the layer. Serial mode
  must stay byte-for-byte the same.
* MTP-R-5.2 Boot flow in `app_main()`: after `io_init()`, if `io_usb_files_mode()` then (a) do not call
  `usb_serial_jtag_driver_install()`, (b) start `tinyusb_driver_install()` with CDC + MTP, (c) register the storages, (d) start
  `cmd_task` on the CDC backend. No recording starts in this branch. The branch must read PFS only.
* MTP-R-5.3 Way back (needed: the board is remote). In files mode there is no USJ auto-reset, and esptool cannot reset a
  TinyUSB CDC port. Give the CDC port two commands: `REBOOT` (calls `esp_restart()`; the next boot reads GPIO 2 again) and
  `DOWNLOAD` (writes `REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)` then `esp_restart()`). IDF itself uses
  this pair of calls in `components/esp_usb_cdc_rom_console/usb_console.c:259`. A 1200-baud "touch" (line coding callback)
  can call the same code. UNVERIFIED on S3 hardware. A software way to leave files mode when GPIO 2 stays low is not
  possible without a reset; the user must set GPIO 2 high (hand switch) or the firmware must allow a RAM flag
  that makes the next boot ignore GPIO 2 (not designed; ask the orchestrator).
* MTP-R-5.4 Add to `src/CMakeLists.txt`: `PRIV_REQUIRES ... esp_tinyusb`. Keep `esp_driver_usb_serial_jtag` for serial mode.
* MTP-R-5.5 The NAND stays read-only in this mode: use `pfs_early_init`, mount, scan (`pfs_scan_run`) and then only
  `pfs_file_count`, file info, and `pfs_read(ctx, file_id, offset, ...)` (`components/pfs/include/pfs.h:164`). PFS has
  random reads by offset, which fits GetPartialObject. UNVERIFIED: whether the PFS mount writes anything
  (journal, scan marks). Check this before the first flash (hard limit: zero NAND writes).

## MTP-R-6 Host behaviour (question 4)

### Windows
* MTP-R-6.1 Windows binds its in-box MTP driver (`wpdmtp.inf`, section `[MTP]`) in two ways: the compatible ID
  `USB\MS_COMP_MTP` (from the Microsoft OS string descriptor at string index 0xEE) or the Still Image class code
  `USB\Class_06&SubClass_01&Prot_01`. Microsoft's WPD blog says Vista and Windows 7 and later accept either; Windows XP
  needs the OS descriptor. A Microsoft Q&A setup log shows both IDs searched. Sources:
  <https://learn.microsoft.com/en-us/archive/blogs/wpdblog/mtp-over-various-transports>,
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/portable/the-mtp-setup-information---inf--file>,
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors>.
  So class 6 / 1 / 1 is enough; no 0xEE string is needed.
* MTP-R-6.2 Composite with CDC: Windows `usbccgp` splits a composite device by interface. The IAD groups the two CDC interfaces.
  CDC-ACM binds to the in-box `usbser.sys` on Windows 10 and later (UNVERIFIED for the exact build; older Windows needs an INF).
  The MTP interface gets its own child and the WPD driver. Teensy ships the same pattern ("Serial + MTP Disk", class 6/1/1 with
  CDC before it): <https://github.com/PaulStoffregen/cores/blob/master/teensy4/usb_desc.c>, release note
  <https://www.pjrc.com/2022/08/>. A forum report says the serial port showed "unknown" on Windows 7 with a Teensy 3.6, so
  expect driver trouble only on old Windows.
* MTP-R-6.3 Pitfalls: (a) Windows caches the driver per VID/PID and per interface set. When the interface set changes,
  change the PID (the Espressif default derives the PID from the enabled classes, which already does this). (b) Do not
  use the VID/PID `303a:1001` (that is the USJ). (c) The interface string should say "MTP". (d) Use a real unique serial
  number string (3.0.0 builds one from the eFuse MAC by default); a device without one gets a new COM port and a new
  portable-device entry per USB port. (e) Windows may ask `GetDeviceInfo` before `OpenSession`; the class driver
  handles this. (f) Keep bulk packet 64 bytes at full speed. (g) The event interrupt endpoint is required (3 endpoints).
* MTP-R-6.4 Vendor-specific alternative (class 0xFF, as Android does): needs the 0xEE string and an MS OS 1.0
  compatible-ID descriptor with ID "MTP" and an interface string that contains "MTP". Not needed here. TinyUSB's
  `mtpd_open()` would need a one-line patch. Not recommended (UNVERIFIED, more risk).

### Linux
* MTP-R-6.5 udev: `/usr/lib/udev/rules.d/69-libmtp.rules` (read on this machine, libmtp 1.1.23) runs `mtp-probe` for devices with
  `bDeviceClass` in `00|02|06|ef|ff`. A composite device with IAD has class 0xEF: it matches. `mtp-probe`
  (<https://github.com/libmtp/libmtp/blob/master/util/mtp-probe.c>) looks for one interface that has a bulk IN, a bulk OUT,
  and an interrupt IN endpoint. Then `libusb-glue.c` (<https://github.com/libmtp/libmtp/blob/master/src/libusb-glue.c>)
  accepts the device when the interface string contains "MTP" (line 338). The class-6 PTP check in that file is disabled
  (`#if 0`), and the 0xEE check runs only for vendor-specific devices. So: the interface string must contain "MTP".
  Both Espressif's default string and the TinyUSB example string ("TinyUSB MTP") do.
* MTP-R-6.6 Tools on this machine: `mtp-detect`, `mtp-files`, `mtp-filetree`, `mtp-getfile` (libmtp 1.1.23), KDE `kio-extras`
  (MTP slave) and `kio-fuse`. No `gvfs-mtp` is installed. These are enough to test (`mtp-detect` prints the descriptor
  checks; `mtp-files` lists handles, sizes, and parent IDs; `mtp-getfile` reads a whole file).
* MTP-R-6.7 ModemManager may probe the CDC-ACM port of an unknown VID/PID for a few seconds. UNVERIFIED here; the DAQ
  already uses `/dev/ttyACM0` without trouble.

### macOS
* MTP-R-6.8 macOS has no MTP support in Finder. Options: OpenMTP (<https://github.com/ganeshrvel/openmtp>; "Android or any other
  MTP devices"; macOS 10.14 or later; the Kalam kernel is for the newest three macOS versions; MIT license),
  Google "Android File Transfer" (old, unstable), and libmtp tools built with Homebrew. UNVERIFIED: none of these
  is confirmed to work with a non-Android responder; OpenMTP claims general MTP support.
* MTP-R-6.9 Known trap: macOS `ptpcamerad` (Image Capture) claims interfaces of class 6 and holds them. Then libusb tools
  get "access denied". Reports and workarounds (kill `ptpcamerad` in a loop; SIP can block it):
  <https://discussions.apple.com/thread/254841739>, <https://developer.apple.com/forums/thread/823965>. So the class 6 choice
  that is best for Windows is a risk for macOS. A class 0xFF interface might avoid `ptpcamerad`: UNVERIFIED guess.
  Honest result: MTP on macOS is not reliable. For a native macOS path use option C.

## MTP-R-7 Other open-source MTP responders (question 5)

| project | what | use for us |
|---|---|---|
| esp_tinyusb MTP layer, <https://github.com/espressif/esp-usb> | ESP-IDF, S3 supported, TinyUSB based, GVfs tested | base of option A |
| TinyUSB example, <https://github.com/hathach/tinyusb/tree/master/examples/device/mtp> | 750-line RAM file system responder | base of option B, reference for op handling |
| MTP_Teensy (KurtE), <https://github.com/KurtE/MTP_Teensy> (MIT, last push 2024-11) | Arduino, Teensy 3.x/4.x; SD, LittleFS, USB | reference for Windows behaviour with Serial + MTP |
| Teensy cores `usb_desc.c` | class 6/1/1 + CDC composite descriptor | reference for descriptors |
| uMTP-Responder, <https://github.com/viveris/uMTP-Responder> (GPL-3.0) | Linux daemon on FunctionFS | not usable on an MCU; reference for op sequences only (GPL: do not copy) |
| Arduino-ESP32 | no MTP file found in the repository tree on 2026-11-11 | none |
| ESP32-S2/S3 community TinyUSB wrappers | MSC and CDC only | none |
No RP2040 MTP responder was found (web search). QEMU `hw/usb/dev-mtp.c` is another responder, in C, with a full object model
(license GPL-2.0; not copied, not read in detail).

## MTP-R-8 Options, ranking, effort, risk (question 6)

### Option A (recommended): esp_tinyusb 3.0.0 + local fork of its MTP layer
* What: depend on `espressif/esp_tinyusb ~3.0.0`; enable CDC and MTP in sdkconfig; use the default composite descriptors; copy
  the component into `components/esp_tinyusb` and use `override_path`.
* Two sub-steps in the fork (small diff): (1) replace both `esp_vfs_fat_info()` calls (`tinyusb_mtp.c:233`,
  `tinyusb_mtp_operations.c:94`) by a call that works for any VFS (for PFS: size from a stub; for SD: keep FATFS info);
  (2) make the layer read-only: StorageInfo AccessCapability 0x0001, ObjectInfo ProtectionStatus 0x0001, remove
  SendObjectInfo, SendObject, DeleteObject, MoveObject, SetObjectPropValue, and the Android edit ops from
  `CFG_TUD_MTP_DEVICEINFO_SUPPORTED_OPERATIONS` (`include/tusb_config.h` lines 168 to 195) and refuse them in
  `tinyusb_mtp_operations.c`.
* Plus a read-only VFS shim for PFS (new file in `src/`, about 200 to 300 lines): `esp_vfs_register("/pfs_ro", ...)` with
  `open`, `read`, `lseek`, `close`, `stat`, `opendir`, `readdir`, `closedir` backed by `pfs_file_count`, file info, and
  `pfs_read`. File names such as `F00012.BIN` (the SD log already uses `F%05uD%d.BIN`, `sd_log.c:91`).
* Storage layout: the stock layer maps one VFS path to one MTP storage. Result: two storages, "NAND" (`/pfs_ro`) and "SD"
  (`/sdcard`, FATFS, mounted by `sd_log.c` with `esp_vfs_fat_sdmmc_mount`). On the host they show as two top-level
  entries. For one storage with two folders in the root (the wording in AGENT_RULES.md), a custom object provider is
  needed instead of the VFS shim (replace `tinyusb_mtp_object_store.c`, 684 lines, with a provider that maps the two root
  folders). Ask the orchestrator which one the user wants. Default guess: two storages, because it needs less code.
* Effort (agent work, guess): 1 day for the first read-only build with fake files; 1 more day for PFS and SD wiring and
  host tests on Linux. Risk: low to medium. Risks: build against IDF 6.1.0 untested (UNVERIFIED); NAND read inside the TinyUSB
  task blocks the CDC port for a few ms; Windows and macOS untested by Espressif.
* First build steps:
  1. `src/idf_component.yml`: add under `dependencies:` the line `espressif/esp_tinyusb: "~3.0.0"` (two-space indent like `pfs:`).
  2. `sdkconfig.defaults` (then delete `sdkconfig.daq`, as the file header says):
     `CONFIG_TINYUSB_CDC_ENABLED=y`, `CONFIG_TINYUSB_CDC_COUNT=1`, `CONFIG_TINYUSB_MTP_ENABLED=y`,
     `CONFIG_TINYUSB_MTP_EP_BUFSIZE=4096`, `CONFIG_TINYUSB_MTP_MAX_STORAGES=2`, `CONFIG_TINYUSB_MTP_MAX_OBJECTS=512`,
     `CONFIG_TINYUSB_DESC_MTP_STRING="IREC DAQ MTP"`. Check the real key names with `pio run -t menuconfig` first
     (names are from the 3.0.0 `Kconfig`; the CDC key names were not all read, UNVERIFIED).
  3. Make a test environment `[env:daq_mtp]` (`build_flags = -DDAQ_MTP_TEST=1`, recording disabled) so that serial builds stay the same.
  4. `src/mtp_files.c`: `tinyusb_mtp_install_driver()`, register the storages, `tinyusb_driver_install(&cfg)` with
     `TINYUSB_DEFAULT_CONFIG()` and a custom string array for the serial number. Start it only when `io_usb_files_mode()`.
  5. Spike before PFS: register a RAM-only fake provider (no NAND access) and test with `mtp-detect`, `mtp-files`,
     `mtp-getfile` and KDE. Then add the PFS shim.
  6. Copy the component and apply the two fork changes only after step 5 works.

### Option B: own MTP responder on the upstream class driver
* What: `espressif/tinyusb 0.21.0~4` (or `esp_tinyusb 2.4.0` with MTP off) plus an own `tusb_config.h`, own descriptors, own
  PHY and task code (copy about 150 lines from `esp_tinyusb/tinyusb.c`), and own operation handlers based on
  `mtp_fs_example.c` (750 lines; drop the write parts, add folders and PFS reads).
* Effort: 3 to 5 days. Risk: medium. Gain: full control, smaller code (about 600 lines), no Espressif RW code to cut out.
  Loss: the object-property operations (0x9801 family) and the many host fixes in Espressif's layer would have to be
  rewritten if Windows or Linux asks for them (UNVERIFIED that they do).
* Use B only if A's fork turns out too tangled.

### Option C: read-only USB mass storage with a synthetic FAT (also the macOS path)
* What: expose the PFS files and SD files as a read-only FAT volume made on the fly (boot sector, FAT, directory entries,
  file data from `pfs_read`). CDC + MSC composite. esp_tinyusb 3.0.0 has MSC with a block-device backend (IDF >= 6.0.4).
* Pros: native on Windows, Linux, and macOS with no driver and no tool. Cons: a synthetic FAT is real work (about 500 to 800
  lines plus tests); hosts cache the volume, so the file list is fixed while the cable stays connected; the host
  may try to write (answer write-protected); one folder per device is easy to model. Effort 3 to 5 days. Risk: medium.
* Use C if the user needs macOS without third-party tools. It does not need MTP at all.

### Option D (not recommended)
* A class 0xFF "Android style" MTP descriptor to avoid `ptpcamerad`. Needs a patch of `mtpd_open()`, the 0xEE string, the
  MS OS 1.0 compat ID, and untested macOS behaviour. UNVERIFIED benefits.

## MTP-R-9 Decisions for the orchestrator and open questions

1. Is "two MTP storages (NAND, SD)" acceptable in place of "one root with two folders"? (Default guess: yes, to start.)
2. Is read-only MSC (option C) wanted as the macOS answer, or is "MTP plus OpenMTP, best effort" enough?
3. How does the board leave files mode when GPIO 2 stays low and nobody can reach it? Proposal: CDC `REBOOT` plus a RAM flag
   that the next boot reads instead of GPIO 2 (needs a design note and a Deviation Log entry).
4. VID/PID: use Espressif VID 0x303A with the default PID 0x4041 for now? A real product needs its own PID
   (Espressif has a PID program: <https://github.com/espressif/usb-pids>, UNVERIFIED current rules).
5. Does the PFS mount write to NAND (journal or scan marks)? It must not. Check in `Paranoid-FS` before the first flash.
6. May the first hardware test flash the `daq_mtp` build with GPIO 2 low and MODE1 low? (The rules need an explicit "recording off"
   build flag; that flag does not exist yet in STATE.md.)

## MTP-R-10 Source index (all read on 2026-11-11)

* TinyUSB master: `src/class/mtp/{mtp.h,mtp_device.h,mtp_device.c}`, `examples/device/mtp/src/*`, `src/device/usbd.h` (macro
  `TUD_MTP_DESCRIPTOR`, line 317), `src/device/usbd.c` (driver table, line 341). Releases API: 0.19.0, 0.20.0, 0.21.0.
* esp_tinyusb 3.0.0 zip: <https://components-file.espressif.com/components/espressif/esp_tinyusb/3.0.0/espressif__esp_tinyusb-v3.0.0.zip>
  (`CMakeLists.txt`, `Kconfig`, `CHANGELOG.md`, `include/{tinyusb.h,tinyusb_mtp.h,tusb_config.h}`, `mtp/*.c`, `usb_descriptors.c`, `tinyusb.c`).
* espressif/tinyusb 0.21.0~4 zip: <https://components-file.espressif.com/components/espressif/tinyusb/0.21.0~4/espressif__tinyusb-v0.21.0_4.zip>.
* IDF 6.1.0 (local): `components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h`, `usb_serial_jtag_ll.h`,
  `components/esp_hw_support/usb_phy/usb_phy.c`, `components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c`,
  `components/esp_usb_cdc_rom_console/usb_console.c`, `components/soc/esp32s3/include/soc/soc_caps.h`.
* Project: `src/cmd.c`, `src/stream.c`, `src/io.c`, `src/main.c`, `src/board.h`, `src/sd_log.c`, `src/CMakeLists.txt`,
  `sdkconfig.defaults`, `src/idf_component.yml`, `platformio.ini`.
* Paranoid-FS: `components/pfs/include/pfs.h` (`pfs_read`, `pfs_file_count`).
* libmtp: `util/mtp-probe.c`, `src/libusb-glue.c`, local `/usr/lib/udev/rules.d/69-libmtp.rules`.
