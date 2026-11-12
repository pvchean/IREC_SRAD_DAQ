# MTP review of the USB files-mode code (static review, no hardware)

Author: fresh reviewer agent. Date: 2026-11-11. Branch `mtp-usb-mode`. Language: ASD-STE100. Labels: MTP-V-n.
The reviewer used no hardware, no /dev/ttyACM*, no flash, no usbreset. Zero NAND writes.
Scope: `src/usb_files.c`, `src/files_mode.*`, `src/io.c`, `src/main.c` (files-mode branch), `src/console_io.*`, `src/mtp_vfs.c`,
`src/mtp_store_{pfs,sd}.c`, the carrier `mtp_usb/` (daq_mtp.defaults, tusb_config.h, usb_descriptors.c, tinyusb_task.c, dcd_dwc2.c).
IDF sources: `~/.platformio/packages/framework-espidf/components` (paths below are relative to it).

## MTP-V-0 Result in short

* Verdict for the first flash of `daq_mtp_force` by a human: SAFE TO FLASH. Every boot path reaches the USB Serial/JTAG (USJ)
  with the PHY on the USJ before TinyUSB starts (MTP-V-B.1). A power cycle always gives the 10 s grace window.
* The biggest open risk is MTP-V-1: after a panic or watchdog reset in files mode, the host does not see a disconnect. The grace window
  is then invisible to the host. The board recovers by itself as 303a:4041 after about 10 s (probable, see MTP-V-1.2), and then CDC `DL` works.
  A replug is necessary only when the firmware crashes again before `DL` can act.
* Changes made by this review (all marked `REVIEW (MTP-V)` in the source, Deviation Log rows added): MTP-V-4 (USB task stack 8192),
  MTP-V-5 (SD store lock for the cmd task), MTP-V-6 (fd slot reuse after a refresh), MTP-V-7 (comment), MTP-V-9 (DIAG line in `LOG`),
  MTP-V-10 (no abort with D+ up).
* Checks after the changes: `pio run -e daq_mtp_force` SUCCESS (RAM 64.7 %), `pio run -e daq` SUCCESS, no warnings.
  `nm -C .pio/build/daq_mtp_force/firmware.elf | grep tud_mtp_`: `T` for the 6 callbacks, `T mtp_dispatch`.
  `tools/mtp_hosttest/run.sh`: 8427 checks passed, 0 failed.

## MTP-V-R Ranked defect list (defects MTP-V-1 to MTP-V-13)

| ID | Risk | Where | Defect | Minimal fix | State |
|---|---|---|---|---|---|
| MTP-V-1 | HIGH | `usb_files.c:84-92`, `usb_files.c:6` (RULE 3); IDF `esp_system/port/soc/esp32s3/system_internal.c:37-69` | A panic or task-watchdog reset does not drop D+. The host keeps a stale 303a:4041. Also the DWC2 internal DMA is not reset by `esp_system_reset_modules_on_exit()` (only GDMA and the SDMMC DMA are reset). | IRAM crash hook that does the `dcd_disconnect()` register writes (MTP-V-1.3). | OPEN, not applied |
| MTP-V-2 | MEDIUM (hard limit) | `main.c:332`, `main.c:337`; `Storage_Design.md` MTP-S-2.4 | `LEAVE` boots the `daq_mtp` serial mode. It mounts PFS read-write (`pfs_mount` can erase a block) and starts `pfs_writer`, SD logger and WiFi. Test step MTP-M-7.5 does `LEAVE`. | Do `LEAVE` only when files-mode `INFO` shows `blocked_writes=0 nor_blocked=0`. Better: in `daq_mtp` also mount serial mode through the read-only HAL. User decision. | OPEN |
| MTP-V-3 | MEDIUM | `mtp_vfs.c:280-284`, `mtp_store_pfs.c:91`, `usb_files.c:218` | The OpenSession hook blocks the TinyUSB task during the refresh (PFS index wait up to 10 s, SD probe, at most 30 s). In that time TinyUSB does not answer EP0 requests and does not read CDC, so `DL`/`REBOOT`/`LEAVE` do not work. Linux control transfers time out at 5 s. If the refresh passes 30 s, the worker still holds the shim lock and the next VFS call blocks the USB task: the watchdog resets the chip (then MTP-V-1). | Reduce `INDEX_WAIT_MS` to 3000 for the MTP refresh, or wait for the index once before the USB start (the scan starts before the grace window). | OPEN (low probability: the scan ends in about 2 s, before the grace ends) |
| MTP-V-4 | MEDIUM | `usb_files.c:237`; `sdkconfig.daq_mtp_force` `CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584` | Stack sizes are not measured. The USB task runs fread -> VFS -> `pfs_read` -> RS decode, and log lines (ring vprintf 200 B + vsnprintf + vprintf) in 6144 B. The main task (3584 B) runs the files-mode mount, the USB start, `ring_dump` and the restart. A stack overflow is a panic (MTP-V-1). | USB task stack 6144 -> 8192 (applied). Measure with the new `LOG` DIAG line (MTP-V-9). If `main` shows less than 600 B free, set `CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144` in `mtp_usb/daq_mtp.defaults`. | PARTLY FIXED |
| MTP-V-5 | MEDIUM-LOW | `cmd.c:44` (old), `mtp_store_sd.c:66-80` | `SDLS` / `CHUNK SD` in files mode called the SD store `present()` from the cmd task with no lock. It can mount, or unmount and `fclose()` the store FILE, while the USB task reads it or the worker refreshes it. The store rule is one caller at a time. | `mtp_vfs_sd_present()` takes the shim lock; `cmd.c` uses it under `DAQ_MTP_USB`. | FIXED |
| MTP-V-6 | LOW | `mtp_vfs.c` `do_refresh()` / `v_open()` | The refresh freed all fd slots. The fork keeps the FILE of an active read. A later `fclose()` of that old FILE could close a NEW file that got the same local fd (the fork opens the new file before it closes the old one, `tinyusb_mtp_read.c:70-91`). Effect: one failed GetObject. | The refresh only bumps `s_gen`. `v_open()` takes a free slot first, then a slot of an old generation. Old handles still fail with EIO. | FIXED |
| MTP-V-7 | LOW (doc) | `usb_files.c:7`; `dwc2_esp32.h:51` | The comment said "4 of 5 IN endpoints". `ep_in_count = 5` counts EP0. All 5 TX FIFOs are used. | Comment corrected. | FIXED |
| MTP-V-8 | LOW | `main.c:231-267` | The grace window starts after the NAND mount (up to 10 tries), the SD init and `cmd_start`. The USJ is alive in all that time, so flashing works. Only the countdown text comes late. | None needed. Know it for the test (MTP-M-7 step 1). | INFO |
| MTP-V-9 | DIAG | `usb_files.c` `ring_dump()` | No way to see stack and heap margins on hardware. | `LOG` now ends with `DIAG heap_free=.. heap_min=.. stack_free_bytes: TinyUSB=.. mtp_vfs=.. cmd=.. main=.. led=..` then `END`. | ADDED |
| MTP-V-10 | LOW | `usb_files.c:241` | `ESP_ERROR_CHECK(tinyusb_cdcacm_init())` aborted after D+ was up: a stale device and a reset loop with no visible grace. | On failure: `usb_files_prepare_restart()` then `esp_restart()` (the proven REBOOT path). | FIXED |
| MTP-V-11 | INFO | boot paths | Every boot path checked: see MTP-V-B.1. | - | OK |
| MTP-V-12 | LOW | `usb_files.c:172` | `HANG` is in the shipped image. One stray `HANG` line resets the chip (and with MTP-V-1 open, leaves a stale device). | Put it under `#ifdef USB_FILES_TEST_HANG` after the MTP-M-7 tests. | OPEN |
| MTP-V-13 | LOW | `usb_files.c:98-106` | `ring_vprintf` writes `s_ring_n++` from many tasks with no lock. Lines can mix. Cosmetic. | Optional spinlock. | OPEN |

### MTP-V-1.1 Why the host keeps a stale device after a crash
* A panic, an interrupt watchdog and the task watchdog end in `esp_restart_noos()` (CPU reset). A CPU reset keeps the USJ, USB_WRAP and DWC2 registers
  and the RTC PHY mux (Spike_Report.md MTP-P-11.1). `esp_system_reset_modules_on_exit()` resets WiFi, timers, SPI01, UART, GDMA, crypto and the SDMMC host
  (comment there: "The DMA inside SDMMC Host needs to be reset to avoid memory corruption after restart"). It does NOT reset USB OTG (`SYSTEM_USB_RST`).
* So the DWC2 stays connected (D+ pull-up on) through the bootloader. At `app_main`, `mux_to_jtag()` gives the PHY to the USJ, whose pull-up is also on.
  D+ never goes low, so the host sees no disconnect and keeps the old 4041 address. The USJ does not answer at that address. 1001 is invisible.
* Side risk: while the DWC2 is still on the bus (reset to `app_main`, about 0.3 to 1 s), an OUT or SETUP packet from the host can be written by the DWC2 DMA
  into the old buffer address in internal SRAM, which the bootloader or the new app now uses. At most one armed OUT per endpoint (MTP 4096 B, CDC 512 B, SETUP 24 B).
  After `mux_to_jtag()` the DWC2 is off the bus, so a second boot is clean. This is a crash, not a brick.

### MTP-V-1.2 Is a replug really needed? (probable answer, verify with the HANG test, MTP-M-7 step 6)
* After the crash the app boots and runs the 10 s grace (invisible to the host). Then `tinyusb_driver_install()` switches the PHY to the OTG and `dcd_init()`
  does a core reset with soft disconnect before `dcd_connect()`. This gives a D+ low time. The host then sees a disconnect and a NEW 303a:4041.
* MTP-P-9.2 supports this: before the `tud_disconnect` fix, `REBOOT` left a stale 4041 "for the whole grace time" and only `DL` stayed stale for ever.
* So: after a crash, wait about 15 s. 4041 comes back with CDC. Then CDC `DL` gives 1001 for a flash. A replug is necessary only if the firmware crashes
  again before `DL` (a crash loop after the grace), or if the host gave up on the port. A replug that does not remove ALL power (for example the board also
  has an external supply) is not a power cycle: remove all power.

### MTP-V-1.3 Fix (not applied: crash-path USB code must be proven on hardware first, AGENT_RULES board-safety rules)
Do the same register writes as `dcd_disconnect()` (`dcd_dwc2.c:591-609`), which ran on hardware in every REBOOT/DL test (MTP-P-9.2). Write only USB_WRAP and
the DWC2, never `USB_SERIAL_JTAG.conf0`. Act only if TinyUSB runs. The boot time after the reset (bootloader, PSRAM test) is the D+ low time; add 100 ms for margin.

```c
/* usb_files.c */
#include "esp_rom_sys.h"
#include "soc/usb_wrap_struct.h"
#include "soc/usb_reg.h"
static void IRAM_ATTR usb_files_crash_disconnect(void)
{
    if (!s_usb_up) { return; }                         /* PHY still on the USJ: write nothing */
    usb_wrap_otg_conf_reg_t c = USB_WRAP.otg_conf;
    c.pad_pull_override = 1; c.dp_pullup = 0; c.dm_pullup = 0; c.dp_pulldown = 1; c.dm_pulldown = 1;
    USB_WRAP.otg_conf = c;                             /* D+ low: the host sees a disconnect */
    REG_SET_BIT(USB_DCTL_REG, USB_SFTDISCON);          /* DWC2 soft disconnect: no more host packets, so no DMA writes */
    esp_rom_delay_us(100000);                          /* < 800 ms interrupt watchdog */
}
void esp_task_wdt_isr_user_handler(void) { usb_files_crash_disconnect(); }   /* weak hook, task_wdt.c:502, called before the panic */
/* Other panics (abort, exception, stack overflow): link with -Wl,--wrap=esp_panic_handler (as bt/controller/CMakeLists.txt:73 does) */
void __real_esp_panic_handler(void *info);
void IRAM_ATTR __wrap_esp_panic_handler(void *info) { usb_files_crash_disconnect(); __real_esp_panic_handler(info); }
```
* Put `target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")` in `src/CMakeLists.txt` under the DAQ_MTP_USB property. Check that
  PlatformIO keeps the flag (`nm` must show `__wrap_esp_panic_handler` called from `panic_handler`). PlatformIO moved `--whole-archive` before (MTP-M-4).
* `s_usb_up` is in internal DRAM; the function is in IRAM; `esp_rom_delay_us` is in ROM. So it works with the cache off.
* After the reset, `mux_to_jtag()` gives the PHY to the USJ with the USB_WRAP override still set. That is the same state as after REBOOT/DL (proven).
  `tinyusb_driver_install()` resets USB_WRAP before the next use (`usb_wrap_hal.c`).
* Test (human at the board): flash, power cycle, then CDC `HANG`. Expect: disconnect at about 8 s, 1001 for the grace, then 4041. Then CDC `DL` (proves the pads).
* `esp_register_shutdown_handler()` does not help here: it runs only in `esp_restart()`, not in a panic. All files-mode `esp_restart()` calls already go
  through `files_mode_restart()` or MTP-V-10.

## MTP-V-B Boot paths and order (question 1 and 2)

### MTP-V-B.1 Every boot path
| Path | What happens | USJ visible to the host? |
|---|---|---|
| Power-on (MODE2 low, or `daq_mtp_force`) | `usb_files_early_init()` first in `app_main` (pads clean: no write; `mux_to_jtag`). Mount, SD init, cmd task, then 10 s grace, then TinyUSB. | Yes, from the bootloader to the end of the grace. |
| Power-on, MODE2 high (switch absent in `daq_mtp`; pin has pull-up) | Serial DAQ (USJ driver installed by `cio_init`). TinyUSB never starts. | Yes, always. |
| MODE2 stuck low | Files mode at each boot; grace at each boot; `LEAVE` gives serial mode once (RTC no-init flag, `io.c:145-149`). | Yes, in each grace. |
| `REBOOT` / `DL` / `LEAVE` (CDC or cmd) | `files_mode_restart()`: 300 ms, `tud_disconnect()` + 500 ms, `mux_to_jtag()`, pad repair, `esp_restart()` (`files_mode.c:60-73`). Proven in MTP-P-9.2. | Yes (DL: ROM download mode in about 4 s). |
| Panic / TWDT / INT WDT in files mode | MTP-V-1. | No, until TinyUSB starts again (about 10 s) or a replug. |
| Panic or hang BEFORE the grace ends | The PHY is still on the USJ. A CPU reset keeps the USJ connected. esptool resets the chip into download mode by the USJ hardware (no app code needed). | Yes. A boot loop here cannot brick the board. |
| `tinyusb_driver_install()` fails | `ESP_ERROR_CHECK` abort. If D+ was not up yet, the PHY switch already dropped the USJ, and the reboot gives 1001 again with a grace. Loop with a visible grace: flashable. | Yes. |
| `tinyusb_cdcacm_init()` fails | Now MTP-V-10: D+ drop, restart, grace. | Yes. |
| `mtp_vfs_init()` / register storage | No block (no file list is read; `esp_vfs_register` only). A failure aborts before the PHY switch: grace again. | Yes. |
| Brown-out | Core reset; USJ conf0 back to default (MTP-P-11.1). | Yes. |
* Pins: no board pin uses GPIO 19/20 (USB) or GPIO 0 (`board.h`). The app cannot break the USB pads by a `gpio_config`.

### MTP-V-B.2 Order of the files-mode start
`files_mode_main()` (`main.c:232-269`): log ring, LED task, `pfs_early_init`, NAND bind, read-only NOR HAL (`nor_ro_littlefs_get`), read-only PFS mount (10 tries),
PFS scan task, store init, SD store init (`mtp_store_sd_init` only sets a flag: NO SD mount at boot), cmd task, then `files_mode_usb_start()` (grace, then USB).
* Good: all storage work is BEFORE the USB start, while the USJ is alive. No card cannot delay the boot: the first SD mount try is in the first OpenSession refresh,
  in the worker task, and then at most every 2 s (`mtp_store_sd.c:22,75`). A no-card mount fails fast (the SDMMC host sees the response timeout in hardware).
* Watchdog during a long read: `tud_task_ext()` returns after at most 16 events (`CFG_TUD_TASK_EVENTS_PER_RUN`, `usbd.c:697-707`), and the loop feeds the TWDT
  after each return (`tinyusb_task.c:87-90`). Each event reads at most 4096 B (`CONFIG_TINYUSB_MTP_EP_BUFSIZE`). A 3 MB GetObject is about 770 events, fed every 16.
  The USB task blocks on its queue during each USB transfer, so IDLE1 also runs. Reads run in the USB task (not in the worker task). Only the refresh uses the worker,
  and the USB task feeds the TWDT while it waits (`mtp_vfs.c:280-283`). A 3 MB GetObject cannot trip the 8 s watchdog unless one 4096 B PFS read takes more than 8 s.

## MTP-V-C Memory and concurrency (question 4 and 5)

* DMA: `CFG_TUD_DWC2_DMA_ENABLE 1` (`tusb_config.h:107`) on the S3. All transfer buffers are static `CFG_TUSB_MEM_SECTION` = `DRAM_DMA_ALIGNED_ATTR`
  (MTP `_mtpd_epbuf`, `mtp_device.c:120`; CDC FIFO buffers). No heap or PSRAM buffer goes to `usbd_edpt_xfer`. `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`: `malloc()` gives
  internal RAM only, so the fork's `malloc` buffers (prop list, paths) are internal. The S3 internal SRAM has no data cache: no cache maintenance needed (`CFG_TUD_MEM_DCACHE_ENABLE 0`).
* Tasks: TinyUSB prio 5, CPU1 (default), now 8192 B. `mtp_vfs` worker prio 3, 8192 B, any core. cmd prio 1, CPU0, 8192 B. main prio 1, CPU0, 3584 B. LED 3072 B.
  `pfs_scan` CPU1 prio 2 (deletes itself). Only TinyUSB and the IDLE tasks feed the TWDT.
* Locks: the shim mutex covers every store call (reads, refresh, now also `SDLS`/`CHUNK SD` presence, MTP-V-5). PFS has its own mutex: `CHUNK PFS`, `LIST`, `FILES`
  from the cmd task use PFS directly and do not touch the store page cache. CDC writes from the cmd task and the main task go through `s_tx_lock`. The CDC
  receive parser runs in the USB task; the cmd task reads a stream buffer (one writer, one reader).
* Remaining concurrency gap: `do_sdls()` / `do_chunk()` SD open `/sdcard` files directly. If the USB side unmounts the card (card removed: CMD13 fails) at the same time,
  the cmd task's FATFS handle becomes invalid. FATFS returns an error; no crash is expected. No card is installed now.
* File size race (question 5): in files mode nothing writes the NAND or the SD, so sizes do not change between refreshes. The fork caches the object size from
  `stat()` and the shim clamps every read to the snapshot size (`mtp_vfs.c:127-128`). A refresh during a read cannot happen: the refresh runs in the USB task
  (OpenSession) and the reads also run in the USB task. After a refresh, old handles fail with EIO, and MTP-V-6 removes the fd reuse collision.
  One real effect: the first OpenSession can come before the PFS index is ready. Then the list is short until the next OpenSession (logged "file index not ready").

## MTP-V-D Descriptors and Windows (question 6)
* Device: bcdUSB 0x0200, class EF/02/01 (IAD) because CDC is on (`usb_descriptors.c:30-37`). Interfaces: IAD {0 CDC comm 02/02/00, 1 CDC data}, 2 MTP 06/01/01.
  Endpoints: 0x81 CDC notify (int, 8 B), 0x02/0x82 CDC bulk 64 B, 0x83 MTP event (int, 64 B, interval 1), 0x04/0x84 MTP bulk 64 B (`usb_descriptors.c:260,275`).
  IN FIFOs: 5 of 5 with EP0 (MTP-V-7). The FIFO RAM (256 words) held this set in the spike enumeration (MTP-P-9.1).
* Windows: usbccgp splits the composite. The IAD function (02/02) binds the in-box `usbser.sys` (Windows 10 and later; Windows 7/8 need an INF).
  Interface 2 (06/01/01) binds the in-box WPD MTP driver (`wpdmtp.inf` matches `USB\Class_06&SubClass_01&Prot_01`); Explorer shows it as a portable device or camera.
  No MS OS 1.0 or 2.0 descriptor is necessary: those are for vendor-class (FF) MTP interfaces. bcdUSB 0x0200 means Windows does not ask for a BOS.
  Windows asks for string 0xEE once per VID/PID/bcdDevice; `tud_descriptor_string_cb` returns NULL for an index out of range (`descriptors_control.c:143`), so EP0 stalls. Correct.
  The serial string (MAC) is unique, so two boards get two instances. Windows caches per 303a:4041 bcdDevice 0x0100: if a stock Espressif TinyUSB example with the same PID
  (CDC+MTP) was used on that PC, the cached driver binding is the same set, so no conflict. UNVERIFIED on a real Windows PC.
* Windows autoplay may open the photo import for a PTP-class device. It reads only.

## MTP-V-E macOS and Linux notes (question 7)
* macOS has no MTP file browser. Use OpenMTP (libusb) or a similar tool. macOS `ptpcamerad` (Image Capture) claims every PTP-class (06/01/01) interface at
  plug-in and opens a session; then other tools get "device busy". Close Image Capture/Photos, set "Connecting this device opens: No application", or stop
  `ptpcamerad` before you start OpenMTP. Image Capture opening and closing can send a USB reset: this tests the P1 recovery path.
* macOS binds the CDC to `AppleUSBCDCACM`: `/dev/cu.usbmodem<serial>1`. IAD composites are supported.
* Linux desktops: gvfs (`gvfsd-mtp` or `gvfs-gphoto2`) opens a session at plug-in and lists all objects (this triggers the refresh). ModemManager can send AT probes
  to `/dev/ttyACM0` for some seconds; the cmd task answers `ERR`. None of the probes match `DL`, `REBOOT`, `LEAVE`, `LOG` or `HANG`.
  For the raw-libusb tests (MTP-M-7 step 7), stop gvfs first, or it holds the interface.

## MTP-V-F What the next agent does
1. First hardware run: follow Merge_Report.md MTP-M-7 unchanged. After step 2, send `LOG` and record the DIAG line (MTP-V-9). If `main` < 600 or `TinyUSB` < 1000 bytes free: STOP, report.
2. Before step 5 (`LEAVE`): continue only if `INFO` shows `blocked_writes=0 nor_blocked=0` (MTP-V-2).
3. After steps 1 to 5 pass: apply MTP-V-1.3 in a separate commit, build, and do the HANG test with the human at the board.
4. Before any release: MTP-V-12 (remove `HANG`), MTP-V-3 (shorter index wait).

Approximate tokens of this review: 190k.
