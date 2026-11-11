# MTP-H Debugging hint for MTP-P-9.3 (static analysis, board not connected)

Author: debugging advisor (Opus), 2026-11-11. This is static work only: source reading plus the host journal. Nothing was flashed or run on the board.
Paths: `F` = `tools/mtp_spike/fork/esp_tinyusb`, `TU` = `F/tinyusb/src`, `IDF` = `~/.platformio/packages/framework-espidf`.

## MTP-H-1 Facts from the host journal (`journalctl -k`, 2026-11-11 01:34 to 01:39)

* 01:34:30: 4041 enumerates. `cdc_acm` binds. `mtp-probe` says "was an MTP device". No gvfs or kmtpd process runs (`pgrep` is empty), so no other host program opened the device.
* 01:34:50: `reset full-speed USB device number 35` (libmtp `libusb_reset_device`). This is about 2 s after `INFO` printed "up 28 s".
  So the FIRST OpenSession failed FAST. It did not wait for the 20 s libmtp timeout. It is most likely an immediate libusb error
  (EPIPE/stall, EPROTO/no handshake), not a NAK timeout. UNVERIFIED: the exact start time of mtp-detect.
* 01:34:51: `cdc_acm ttyACM0` binds again. So after the reset, the device ANSWERED GET_DESCRIPTOR and SET_CONFIGURATION: `tud_task` was still alive.
* Then the second OpenSession failed and CDC `INFO` got no answer. At 01:36:14 `usbreset` gave `device descriptor read/64, error -110` (3 times),
  and then `not accepting address` and `unable to enumerate`. The log shows NO disconnect and no new device between 01:36 and 01:39.
* Conclusion A: the chip did NOT reboot. A panic or INT-WDT reboot runs `mux_to_jtag()` and the 10 s grace, and the host would then see
  1001 or a fresh 4041 after a port reset.
* Conclusion B: the USB ISR still runs (a SETUP is ACKed in hardware, and DMA mode re-arms EP0 in the ISR), but `tud_task` does not process
  events. GET_DESCRIPTOR data and the SET_ADDRESS status are made in `tud_task`, so -110 / -62 follow.
  So: the TinyUSB task is BLOCKED or SPINNING with interrupts enabled. The INT WDT (300 ms) does not see this.
  The TWDT sees only a spin (IDLE1 starves), and it only prints because `CONFIG_ESP_TASK_WDT_PANIC` is not set (`sdkconfig.spike_force`).
  The print goes to the USJ console, which has no pads in files mode.

## MTP-H-2 Root causes, ranked

### 1. TinyUSB 0.21.0 MTP class driver: bugs in transaction recovery (HIGH, two of them are confirmed by upstream fixes)
Upstream master (`hathach/tinyusb` `src/class/mtp/mtp_device.c`, read 2026-11-11, after PR #3956 "fix data phase completion and
transaction recovery") changed exactly these spots:
* a. `TU/class/mtp/mtp_device.c:346-361` `MTP_REQ_RESET` (0x66): in the SETUP stage the code clears stalls and then falls to `return true` (line 377)
  with NO `tud_control_status()`. The control transfer never gets its status stage. The host times out (-110) after 5 s.
  Upstream now calls `tud_control_status(rhport, request)` there ("otherwise the request ... hangs").
* b. `mtp_device.c:420` COMMAND phase: `TU_VERIFY(ep_addr == ep_out && type == COMMAND_BLOCK)`. On a ZLP, a short garbage packet, or a late
  IN completion, the callback returns false. Nothing re-arms EP OUT, so the bulk pipe is dead (the host write NAKs forever)
  until the next SET_CONFIG. Upstream: an IN completion or ZLP re-arms the OUT EP, and a bad type sets ERROR and halts both bulk EPs.
* c. `mtp_device.c:193-196` `prepare_new_command()` arms EP OUT without a busy check. The callers are the RESET ACK (line 358) and the RESPONSE complete
  (line 503). After DEVICE_RESET the OUT EP is normally still armed, so `usbd_edpt_xfer` hits `TU_ASSERT(!BUSY)` (`TU/device/usbd.c:1587`).
  Upstream: `if (usbd_edpt_busy(...)) return true;`.
* d. `mtp_device.c:274-277` `mtpd_reset()` clears only the TinyUSB state. esp_tinyusb is not told about a bus reset (`F/tinyusb.c:62-99`:
  only mount/umount/suspend/resume). So `session_open` stays true after libmtp's reset. The second OpenSession then gets 0x201E (SessionAlreadyOpen).
  libmtp accepts that, so this is a smell, not the fault.
* e. `mtp_device.c:512-516` ERROR phase calls `usbd_edpt_stall()` -> `dcd_edpt_stall()` -> `edpt_disable()` (see cause 3), in task context.

### 2. TinyUSB debug output to a USJ console that has no pads (MEDIUM, cheap to remove)
* `sdkconfig.spike_force`: `CONFIG_TINYUSB_DEBUG_LEVEL=1`. `F/include/tusb_config.h:137-138`: `CFG_TUSB_DEBUG_PRINTF esp_rom_printf`.
  At level 1, each failed `TU_ASSERT` prints (for example 1c above, and `mtp_device.c:501` `TU_ASSERT(ep_addr == ep_in)` in the RESPONSE phase).
* `esp_rom_printf` goes to the ROM putc of the USB Serial/JTAG (`IDF/components/esp_rom/patches/esp_rom_usb_serial.c:14-21`
  calls the ROM `usb_serial_device_tx_one_char` and `..._tx_flush`). In files mode the PHY belongs to OTG, so the USJ FIFO never drains.
  UNVERIFIED: whether the S3 ROM tx function busy-waits with no timeout. If it does, the first ASSERT print after 64 bytes hangs `tud_task`
  for ever, with interrupts on. That gives exactly the signature of MTP-H-1.
  (ESP_LOG is safe: `IDF/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c:78,148-169` gives up after 50 ms.)
* Fix: `CONFIG_TINYUSB_DEBUG_LEVEL=0` in files mode. If you need TinyUSB logs, set `CFG_TUSB_DEBUG_PRINTF` to a function that writes the RAM trace (MTP-H-3, step 2).

### 3. DWC2 port: unbounded busy-waits (MEDIUM as the hang mechanism, the trigger is unknown)
* `TU/portable/synopsys/dwc2/dcd_dwc2.c:337-386` `edpt_disable()`: `while ((diepint & INEPNE) == 0){}` (353), `while (!EPDISD){}` (357),
  `while ((gintsts & BOUTNAKEFF) == 0){}` (375, comment: "If this while loop never finishes, we have bigger problems"), `while (!EPDISD){}` (379).
  None of these loops has a timeout. The callers in task context are `dcd_edpt_stall` (MTP ERROR phase, SET_FEATURE(HALT), EP0 stall on an unsupported request)
  and, in ISR context, `handle_epout_dma` (1061-1063) for EP0 IN.
  Race: `handle_ep_irq` (1155-1158) reads DxEPINT and writes ALL the bits back to clear them, also EPDISD and INEPNE, which are not in the mask.
  If an XFRC or SETUP interrupt of the same EP arrives while the task waits in `edpt_disable`, the ISR can clear the bit that the task waits for.
  The task then spins for ever. In ISR context the INT WDT would reboot the chip. In task context only the TWDT (no panic) sees it.
* DMA mode is on (`F/include/tusb_config.h:107` `CFG_TUD_DWC2_DMA_ENABLE 1`). There are fewer field reports for DMA than for slave mode on the S3.
  To test: set it to 0 in the fork (slave mode). It is a one-line A/B test.

### 4. Things that were checked and are probably NOT the cause
* EP and packet sizes: FS bulk 64 B and `CFG_TUD_MTP_EP_BUFSIZE 4096` (Kconfig range 4096-8192 on S3, `F/Kconfig:227-228`). 4096 = 64 x 64 B packets.
  OUT is always armed with 4096 B (`mtp_device.c:195`); a 16 B OpenSession is a short packet and ends the transfer. S3 GHWCFG3: XferSize 16 bits,
  PktCnt 7 bits (max 127) (`dwc2_info.md`). OK. The IN ZLP rule uses `ep_sz_fs` = 64 (`mtp_device.c:306-308,439,459`). OK.
* Interrupt EP: present, EP3 IN, 64 B, bInterval 1 (`F/usb_descriptors.c:275`). `mtpd_open` opens it (295-299) but never arms it. That is normal.
  libmtp needs it only in the descriptor (3 EPs).
* IN EP count and FIFO (`dwc2_esp32.h:51`: ep_in_count 5, dfifo 256 words): EP0, 1, 2, 3, 4 IN = 5, EXACTLY at the limit
  (`dcd_dwc2.c:257` asserts `< 5` before increment). Words: 256 - 14 (DMA EP info) = 242; RX 62; TX 16+2+16+16+16 = 66; total 128. OK.
  RULE: never add a 6th IN endpoint (no HID or second CDC next to MTP). The enumeration and cdc_acm bind also prove that SET_CONFIG passed `dfifo_alloc`.
* Class requests: CANCEL 0x64 (`mtp_device.c:328-335` -> `F/mtp/tinyusb_mtp_transport.c:245`) OK. GET_DEVICE_STATUS 0x67
  (`mtp_device.c:363-370` -> `transport.c:287`, returns {4, 0x2001}) OK. It never reports stalled EPs, which is a smell.
  DEVICE_RESET 0x66: BROKEN, see 1a. GET_EXT_EVENT 0x65: weak stub, stalls. OK.
* VFS fork handlers: GetDeviceInfo / OpenSession / GetStorageIDs take only `mtp_lock` (non-recursive mutex, `F/mtp/tinyusb_mtp.c:92-99`)
  and do not nest. No path found that locks twice. The fake VFS (`src/fake_vfs.c`) does not block. OpenSession is the first libmtp op
  (`ptp_opensession`). GetDeviceInfo comes after it, so GetDeviceInfo stack use (the `tud_mtp_device_info_t` on the stack in `mtp_device.c:530`, 4096 B
  task stack `F/include/tinyusb_default_config.h:80`) cannot explain the FIRST failure. A stack overflow would also panic and reboot. Raise the stack to 6144 anyway.
* Spike app: `src/main.c:65-87` `on_rx` runs in the TinyUSB task (`F/tinyusb_cdc_acm.c:90`), and `cdc_puts` calls `tinyusb_cdcacm_write_flush(...,200 ms)`
  (`F/tinyusb_cdc_acm.c:271-285`, `vTaskDelay(1)` loop). This blocks the whole USB stack for up to 200 ms per line (LOG = many lines), but it ends.
  Fix it in the real firmware: queue the CDC work to another task.
* Host: no autosuspend rule for 303a, ModemManager is not seen in the journal, gvfs and kmtpd do not run.

## MTP-H-3 Test and fix plan for the next agent (in this order, once the board is back)

Rule: never run `mtp-detect`, `mtp-files`, or `usbreset` before steps 1-4 pass. The libmtp reset path is what killed the port.

1. Self-recovery first (MTP-H-4). Without it, each hang costs a human replug. Flash it in the 10 s grace window.
2. Add a RAM trace that survives a reset: `RTC_NOINIT_ATTR` ring (2 KB) plus a magic word. Write one 8-byte record per event from:
   `mtpd_xfer_cb` entry (ep, phase, len, result), `mtpd_control_xfer_cb` (bRequest, stage), `prepare_new_command` (ret),
   `tud_mtp_command_received_cb` (op, tid, resp), `edpt_disable` entry/exit (ep), and the BUS_RESET/SETUP events in `tud_task_ext`.
   At the next boot, in the grace window, `printf` the ring to the USJ (that console works there). After a TWDT reboot, the last records show where the task stopped.
   Also: set `CONFIG_TINYUSB_DEBUG_LEVEL=0` (cause 2) and `CONFIG_TINYUSB_MTP_TRACE_WRITES=y` (it goes to ESP_LOG, so to the CDC `LOG` ring).
3. Raw libusb client (`tools/mtp_raw.c`, `gcc -O2 mtp_raw.c -lusb-1.0`). Outline:
   ```c
   libusb_init(NULL); h = libusb_open_device_with_vid_pid(NULL, 0x303a, 0x4041);
   libusb_claim_interface(h, 2);                 /* do NOT call libusb_reset_device, set_configuration, or clear_halt */
   /* A: GET_DEVICE_STATUS, class IN to interface 2 */
   r = libusb_control_transfer(h, 0xA1, 0x67, 0, 2, buf, 20, 1000);   print r and the bytes
   /* B: OpenSession: len 16, type 1, code 0x1002, tid 0, param 1 (all little endian) */
   uint8_t cmd[16] = {16,0,0,0, 1,0, 0x02,0x10, 0,0,0,0, 1,0,0,0};
   r = libusb_bulk_transfer(h, 0x04, cmd, 16, &n, 2000);   print r (libusb_error_name), n
   r = libusb_bulk_transfer(h, 0x84, rsp, 512, &n, 2000);  print r, n, hex dump (expect 12 B: 0c000000 0300 0120 00000000)
   /* C: GetDeviceInfo, tid 1, code 0x1001, len 12: read data (type 2) until a short packet, then the response */
   /* D: GetStorageIDs 0x1004 tid 2, then CloseSession 0x1003 */
   /* E (only after A-D pass): DEVICE_RESET 0xA1->0x21, bRequest 0x66, wLength 0 -> expect r == 0 within 1 s. Today expect -7 (timeout), bug 1a */
   libusb_release_interface(h, 2); libusb_close(h);
   ```
   Log for every call: wall time (ms), libusb return name, byte count, and hex. Then run `INFO` on the CDC after each step (is the task alive?).
   Interpretation: `LIBUSB_ERROR_PIPE` on B = EP stalled (ERROR phase); `LIBUSB_ERROR_IO` fast = no handshake (EP not active or DMA problem);
   `TIMEOUT` on the write = OUT not armed (bug 1b/1c); write OK and `TIMEOUT` on the read = command lost or response not sent (read the RAM trace).
4. Apply patch P1 (MTP-H-5), rebuild, and repeat step 3. If B still fails, A/B test `CFG_TUD_DWC2_DMA_ENABLE 0` (slave mode) with no other change.
5. Only when step 3 passes 20 times in a loop: run `LIBMTP_DEBUG=USB mtp-detect` (it prints each USB call), then `mtp-files` and `mtp-getfile`.
6. Then the bus-reset test: in the raw client, call `libusb_reset_device(h)` after D, then repeat B. It must pass, and the CDC must stay alive.
7. Record every run in Spike_Report MTP-P-9.x (AGENT_RULES).

## MTP-H-4 Self-recovery without a human

Which watchdog:
* INT WDT (MWDT1): on already (300 ms). It catches only ISR or critical-section hangs. Keep it.
* TWDT (MWDT0): the right tool for this hang. Set `CONFIG_ESP_TASK_WDT_PANIC=y` and `CONFIG_ESP_TASK_WDT_TIMEOUT_S=8` in the files-mode sdkconfig.
  Subscribe the TinyUSB task and feed it from the task loop. `tud_task()` blocks for ever on its queue, so use `tud_task_ext(1000, false)`.
  Then the loop wakes at least 1 time per second when idle. A spin, a deadlock, or a blocking ROM print in any USB callback stops the feed, and the chip panics and reboots in 8 s.
  The IDLE check already catches spins. Only the subscription catches a blocked task.
  Long MTP work (a NAND scan for GetObjectHandles) must call `esp_task_wdt_reset()` in its loop.
* RTC WDT: the last line of defence (if the panic handler itself hangs, the panic path arms it already). Optional: `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y`
  and feed with `wdt_hal_feed()` from the same loop. A stage action of RTC reset also resets the PHY mux. Not needed for the first step.

The host must SEE the reboot. On a panic reboot, shutdown handlers do not run, and the D+ pull-up stays on (MTP-P-9.2). So force a disconnect at BOOT:
before `mux_to_jtag()` in `app_main`, override the pulls of BOTH pad owners to "D+/D- pull-down, no pull-up" for 200 ms, then release.
After that, the grace window enumerates 303a:1001 cleanly and the next agent can flash or read the RAM trace.
Keep `tud_disconnect()` + 500 ms before each software restart (MTP-P-9.2), also in an `esp_register_shutdown_handler()`, which covers `esp_restart()` from any task.

## MTP-H-5 Patches (proposed; the sources were NOT edited)

P1 `F/tinyusb/src/class/mtp/mtp_device.c` (back-port of the upstream fixes 1a, 1b, 1c; the logic is the same as upstream master):
```c
static bool prepare_new_command(mtpd_interface_t* p_mtp) {
  p_mtp->phase = MTP_PHASE_COMMAND;
  if (usbd_edpt_busy(p_mtp->rhport, p_mtp->ep_out)) {
    return true;                       // a read is already armed, it will receive the command
  }
  if (!usbd_edpt_xfer(p_mtp->rhport, p_mtp->ep_out, _mtpd_epbuf.buf, CFG_TUD_MTP_EP_BUFSIZE, false)) {
    p_mtp->phase = MTP_PHASE_ERROR;
    return false;
  }
  return true;
}
...
    case MTP_REQ_RESET:
      if (stage == CONTROL_STAGE_SETUP) {
        if (usbd_edpt_stalled(rhport, p_mtp->ep_out)) { usbd_edpt_clear_stall(rhport, p_mtp->ep_out); }
        if (usbd_edpt_stalled(rhport, p_mtp->ep_in))  { usbd_edpt_clear_stall(rhport, p_mtp->ep_in); }
        return tud_control_status(rhport, request);          // was missing: no status stage -> host timeout
      } else if (stage == CONTROL_STAGE_ACK) {
        prepare_new_command(p_mtp);
        return tud_mtp_request_device_reset_cb(&cb_data);
      }
      break;
...
    case MTP_PHASE_COMMAND: {
      if (ep_addr == p_mtp->ep_in || xferred_bytes == 0) {   // late IN completion or ZLP: listen again
        (void) prepare_new_command(p_mtp);
        break;
      }
      if (xferred_bytes < sizeof(mtp_container_header_t) ||
          p_container->header.type != MTP_CONTAINER_TYPE_COMMAND_BLOCK) {
        p_mtp->phase = MTP_PHASE_ERROR;                       // halt both bulk EPs below; host recovers with 0x66
        break;
      }
      memcpy(&p_mtp->command, p_container, sizeof(mtp_container_command_t));
      ... (rest unchanged)
```
Better: replace the whole file with upstream master `mtp_device.c`, if its header `mtp_device.h` API is still compatible with esp_tinyusb's
`F/mtp/*.c` (check the callback names, `tud_mtp_cb_data_t`, and `MTP_PHASE_*`; master has a new `MTP_PHASE_DATA_COMPLETE`). Record it in the Deviation Log.

P2 `F/tinyusb_task.c:84-86` (TWDT feed, `#include "esp_task_wdt.h"`):
```c
    esp_task_wdt_add(NULL);               // subscribe the TinyUSB task (needs CONFIG_ESP_TASK_WDT_INIT=y, it is)
    while (1) {
        tud_task_ext(1000, false);        // wake at least 1 time/s so that an idle stack still feeds
        esp_task_wdt_reset();
    }
```

P3 `tools/mtp_spike/src/main.c` app_main, first lines (`#include "hal/usb_wrap_ll.h"`, `"hal/usb_serial_jtag_ll.h"`, `"soc/usb_wrap_struct.h"`):
```c
static void usb_force_disconnect(uint32_t ms)
{
    const usb_wrap_pull_override_vals_t w = { .dp_pu = false, .dm_pu = false, .dp_pd = true, .dm_pd = true };
    const usb_serial_jtag_pull_override_vals_t j = { .dp_pu = false, .dm_pu = false, .dp_pd = true, .dm_pd = true };
    usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &w);
    usb_serial_jtag_ll_phy_enable_pull_override(&j);
    esp_rom_delay_us(ms * 1000);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_wrap_ll_phy_disable_pull_override(&USB_WRAP);
}
static void usb_shutdown(void) { tud_disconnect(); esp_rom_delay_us(500000); }
/* app_main: */ usb_force_disconnect(200); mux_to_jtag(); ...
/* files_mode, after tinyusb_driver_install: */ esp_register_shutdown_handler(usb_shutdown);
```
UNVERIFIED: whether the USB_WRAP bus clock is on at that early point after a panic reboot. If writes to `USB_WRAP` have no effect, call `_usb_wrap_ll_enable_bus_clock(true)` first
(`IDF/components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h:232`). Test it: trigger `abort()` from a CDC command and watch `journalctl -k` for a disconnect.

P4 `sdkconfig` for files mode: `CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=8`, `CONFIG_TINYUSB_DEBUG_LEVEL=0`,
TinyUSB task stack 6144 (`tinyusb_config_t.task.size`). For the A/B test only: `F/include/tusb_config.h:107` `CFG_TUD_DWC2_DMA_ENABLE 0`.
