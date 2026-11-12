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
| 00:05 | research | sonnet | MTP options | ~280k | done: Research.md (commit dcbd15c). Plan A: esp_tinyusb 3.0.0 + local fork + read-only VFS shim |
| 00:15 | storage | sonnet | PFS read API, mtp_store, build | ~190k | done (186b5c2,94be6cd,f84efd2). PFS mount can write NAND -> HAL wrapper blocks writes. File names NAND_F<hex>.BIN / uSD_F<hex>.BIN |
| 00:35 | integ | sonnet | boot branch, console_io, daq_mtp env, no-format NOR HAL | running | |
| 00:15 | spike | sonnet | standalone MTP+CDC on board | running | |

## Status
Decisions (orchestrator, 00:15): plan A. ONE MTP storage with root folders NAND and SD (user asked folders in root, not two drives).
VID/PID 303A/4041 ok. Leave files mode by CDC `REBOOT` / `DL` (ROM download). macOS = MTP + OpenMTP (option C, read-only MSC, only if time).
Open: does PFS mount write to NAND? (storage agent checks). `daq_mtp` build must disable recording before any flash of the real firmware.

Build: use ~/.platformio/penv/bin/pio (system pio lacks intelhex). Keep NAND_/uSD_ prefixes. Custom no-format NOR HAL wanted.

## Checkpoint 00:3x (window A ended early: user reported 5 % left; orchestrator had no usage meter and was late)
Agents told to pause and write Spike_Report.md / Boot_Integration.md. Window A total agent tokens ~ 480k + spike/integration (unknown, see their reports).
Resume: a session cron fires at 06:23 and starts fresh agents from these files. It lives only while the VS Code session stays open.

## Spike agent report (00:4x, ~170k tokens, commits 84fe009, 4fccca6)
* The board did NOT run the DAQ firmware. It ran a PFS-PIL test firmware ("@L:I:PFS-PIL READY 64 16 2048"). The spike overwrote
  bootloader, partition table and app only (pfs_nor and NAND untouched). That old firmware is gone; the user should know.
* Board now runs the SPIKE firmware (serial mode, MODE2 reads HIGH, recoverable with penv pio upload, no BOOT button).
  DECISION: leave the spike firmware on the board. Do NOT flash `-e daq` (auto-record at boot if MODE1 high = NAND writes).
* esp_tinyusb 3.0.0 builds on IDF 6.1 / platform 7.1.3. `override_path` fork works (do not name the folder `components/`).
  Fork: plain VFS as storage; read-only; write ops return 0x200E/0x200F. NOT host-tested yet.
* Untested and critical: lsusb, libmtp, GetPartialObject, speed, CDC `DL` and `REBOOT` (the way back from files mode).
* Next: Spike_Report.md MTP-P-7: flash `spike_force`, wait 10 s grace, lsusb -v; test REBOOT, then DL, before anything else.
* Ledger: window A agent tokens total ~ 190k+190k+280k+170k = ~830k (integration agent still unreported).

## Integration agent report (~200k tokens, WIP commit 73912f9)
* `-e daq` and `-e daq_mtp` build. Not flashed. Done: console_io (cio_*), files_mode, io_rec_disable + RTC force-serial flag,
  nor_ro (no-format read-only LittleFS NOR HAL), main.c files-mode branch (DAQ_MTP + MODE2 low), `daq_mtp` env (REC ON refused),
  files-mode command allowlist + LEAVE. Details: Boot_Integration.md.
* Not done: mtp_store_test.sh run, Deviation Log + README/board.h lines, weak-symbol link check (`nm` on the ELF; else WHOLE_ARCHIVE
  or #ifdef), real read-only mount on the board, `DL` command (spike has it; merge it).
* Window A agent total ~ 1.03M tokens (research 280k, storage 190k, spike 170k, integration 200k, + orchestrator).
* Next (window B): (1) spike host tests, REBOOT then DL first. (2) Merge the spike fork + CDC into the daq_mtp build: strong
  files_mode_usb_start(), cio_set_backend(CDC), mtp_store backends behind the VFS shim. (3) Test on Linux with the board, read-only.
  (4) Deviation Log, README. (5) Final summary at 10:45.

## 01:35 Haiku (low) report, commit 164a590
* `daq_mtp` builds; `files_mode_usb_start` is W (weak default) in the ELF. The weak->strong override test (MTP-B-6) is still TODO: do it when the USB code adds the strong symbol.
* tools/mtp_store_test.sh PASS (blocked NAND 3, NOR 1). Deviation Log.md created. README MODE2 line updated.
* 01:27 running: sonnet spike TEST agent (REBOOT/DL first, then libmtp, GetPartialObject, speed, write refusal, CDC alongside MTP).

## 01:45 Spike TEST report (~120k tokens, commits a6004ca, e7f0fde)
* PASS: spike_force flashes; lsusb: 303a:4041 FS, IAD, CDC ACM + PTP class 6/1/1 "IREC DAQ MTP"; CDC INFO works in files mode.
* Way back: REBOOT/DL left D+ pulled up (host saw a stale dead device). FIX: tud_disconnect() + 500 ms delay before restart
  (in tools/mtp_spike/src/main.c; must go into the real firmware). After the fix DL -> 303a:1001 in 4 s, esptool flash works, no BOOT.
* FAIL: libmtp mtp-detect finds the device but PTP_ERROR_IO "failed to open session"; libmtp's USB reset hung the firmware; the host
  xhci now says "device not accepting address", usb_hub_wq kworker stuck in D. NO /dev/ttyACM0, NO 303a in lsusb. No root -> cannot recover.
  >>> BOARD NEEDS A HUMAN REPLUG (or RESET) <<< Until then NO hardware tests. Board firmware: spike_force (with fix), hung in files mode.
  After a replug: 10 s grace on USB-Serial-JTAG; plain `pio run -e spike -t upload --upload-port /dev/ttyACM0` works in that window.
* Lesson: never run mtp-detect first. Use a raw libusb client (gcc+libusb headers exist, no pyusb) + CONFIG_TINYUSB_MTP_TRACE_WRITES + CDC LOG ring.
* Rule added: before any host-side MTP test, the firmware must have a hardware watchdog that resets the chip when the USB stack hangs.
* 01:45 started: OPUS static review (no hardware): Docs/MTP/Opus_Hint.md (root causes, test plan, watchdog self-recovery).
* Ledger window A+: haiku 43k, spike-test 120k (est. total since 00:00: ~1.2M agent tokens).

## 01:55 Opus hint (commit 23186a2, static review). Board is back (user replugged at ~01:49): 303a:4041, spike_force, files mode.
* Likely cause: TinyUSB 0.21.0 mtp_device.c recovery bugs (DEVICE_RESET status stage never answered, dropped ZLP/wrong-type packet leaves
  the bulk pipe dead, no busy check -> assert); debug level 1 assert prints to a dead console; dcd_dwc2 edpt_disable loops without timeout.
  Ruled out: 64 B vs 4096 B buffer, interrupt EP, FIFO size, VFS locks. IN endpoints at the limit (5): never add a 6th.
* Plan: patches P1 (back-port), P2 (task WDT panic, fed by USB task), P3 (D+/D- low at boot), P4 (sdkconfig). Prove WDT with a CDC `HANG` command
  BEFORE any MTP test. Then raw libusb client (no mtp-detect first, no usbreset).
* 01:55 running: sonnet FIX agent (does the above in that order).

## 02:15 FIX agent (commits f99bfc3, 76da66b): details in Spike_Report.md MTP-P-10
* P1, P2, P4 built; WDT PROVEN (HANG -> reset in 8 s -> host sees disconnect and 4041 again). ptp_probe.c written (compiles, not run).
* P3 v1 broke the USB Serial/JTAG pad: board is INVISIBLE (port "not attached"). HUMAN NEEDED: hold BOOT + replug, then flash spike_force (v3, fixed, untested).
  Recipe and test order: MTP-P-10.4. No MTP host test has run yet.

## 02:10 FIX agent report (~130k tokens): WDT PROVEN, board LOST again (needs BOOT button)
* P1 (mtp_device recovery back-port), P2 (TWDT fed by USB task, silent panic reboot), P4 (sdkconfig) work. CDC `HANG` -> chip resets itself in ~8 s,
  re-enumerates as 303a:4041 in ~10 s, no human. 
* P3 v1 (D+/D- low at every boot) killed the USB-Serial-JTAG pad: grace window gone, then `DL` dropped the board. Agent noticed and continued (mistake; rule added).
* Board: flash = v1 (buggy), in ROM download mode, no D+ pull-up, no /dev/ttyACM0. RECOVERY (human): hold BOOT, plug in, check `lsusb` shows 303a:1001, then
  `cd tools/mtp_spike && ~/.platformio/penv/bin/pio run -e spike_force -t upload --upload-port /dev/ttyACM0` (builds+flashes v3: usj_pads_restore(), P3 only after crash reset; untested).
* Written: tools/mtp_spike/host/ptp_probe.c (raw libusb, stages 1-7 + -r), host_cdc.py, HANG/ABORT cmds. Not run. Test order after recovery: Spike_Report.md MTP-P-10.4 (a power cycle, b DL, c HANG, then MTP stages).
* Hardware tests BLOCKED until a human holds BOOT. Non-hardware work meanwhile: see next entries.

## 02:43 heartbeat: no agent running; board still missing (needs BOOT). Started a static review agent (sonnet) of the v3 pad patch (MTP-P-11), no hardware.

## 03:0x Static review of v3 done (~110k tokens, commit 8738b93): verdict SAFE for default v3 (UNVERIFIED items in MTP-P-11.6).
* usj_pads_restore() runs first in app_main (writes only if conf0 is dirty). v1 pad-override disconnect deleted. P3 now behind -DSPIKE_P3 (default 0, env spike_force_p3).
  Without P3: a crash/WDT reset leaves the host a stale device, but a plain replug gives a clean boot + 10 s grace.
* Builds: spike_force, spike, spike_force_p3 OK. Not flashed. Board still missing (needs human with BOOT). Recovery + test order: Spike_Report.md MTP-P-11.7.
* Window total agent tokens since 00:00: ~1.75M (incl. opus 247k).

## 03:2x MERGE report (~300k tokens; commits 85e715a..9bc88c5; Merge_Report.md MTP-M-1..9)
* `-e daq_mtp`, `-e daq_mtp_force` (files mode without the MODE2 switch), `-e daq` build clean; `daq` unchanged. Host test PASS. Build wiring: -DDAQ_MTP_USB=ON (cmake_extra_args) adds mtp_usb/ (fork + carrier), daq_mtp.defaults, dependencies.lock.daq_mtp.
* CRITICAL FINDING (MTP-M-4): in the spike and first daq_mtp build the fork's strong tud_mtp_*_cb callbacks were NOT linked (weak defaults return -1; mtp_dispatch absent) -> every MTP command failed. Very likely the real cause of "failed to open session". Fixed in the merged fork with a link anchor in tinyusb_mtp_install_driver(). CHECK EVERY BUILD: `nm -C firmware.elf | grep tud_mtp_` must show T.
* New: src/usb_files.c (grace window, usj_pads_restore, CDC console via cio_set_backend, log ring, DL/REBOOT/LEAVE/LOG/HANG handled in the USB layer), src/mtp_vfs.c (read-only VFS: NAND + SD folders, refresh at OpenSession only).
* Untested on hardware: everything. Hardware order MTP-M-7: flash daq_mtp_force (human with BOOT), power-cycle grace, INFO/LOG, DL+reflash, REBOOT, LEAVE, ptp_probe -s1..3 (stages 4-6 need a rewrite for real files), mtp-detect last.
* TODO non-hardware: ptp_probe stages 4-6 for real files; remove the 4.9 MB duplicate fork copy (spike's) or document; P3 not merged (stale device after WDT reset: replug).
* Agent tokens since 00:00: ~2.2M.

## 03:2x ptp_probe generalised (haiku-class task, commit d1f0be7): real-file stages 3-6 + --fake mode. Compiles; untested on hardware.
## STATUS: ALL NON-HARDWARE WORK IS DONE. WAITING FOR THE BOARD.
* Heartbeat rule: run `lsusb | grep 303a`. If NO 303a device: do nothing (no agents, no tokens), end the turn.
* If 303a:1001 appears: it may be ROM download mode (human held BOOT) or the grace window. Start ONE fresh sonnet HARDWARE agent that follows
  Merge_Report.md MTP-M-7 / Spike_Report.md MTP-P-11.7 strictly: flash `daq_mtp_force` (penv pio, `-e daq_mtp_force -t upload`), power-cycle proof, INFO/LOG,
  DL+reflash, REBOOT, LEAVE, then ptp_probe -s 1..6, mtp-detect only last. STOP at the first anomaly (Board-safety rules). Check `nm -C firmware.elf | grep tud_mtp_` shows T.
* If 303a:4041 appears (board running old spike v1/v3?) read Spike_Report.md to see which firmware; do not run mtp-detect.
* Zero NAND writes. Agent tokens since 00:00: ~2.6M.

## 06:25 Window B started (resume job). Board still missing (no 303a). Started sonnet HOST-HARNESS agent (tools/mtp_hosttest): real MTP handler code + mocked transport + scripted PTP session; fixes bugs it finds. Report: HostTest_Report.md.
* Token rule for window B: stop launching agents when window-B agent tokens pass ~2.6M or at 11:00.

## 06:5x HOST HARNESS report (~450k tokens; commits e6032cf, 320ab75, bc38e7a; HostTest_Report.md MTP-H-1..7)
* `tools/mtp_hosttest/run.sh` (20 s): real fork mtp/*.c, real mtp_device.c (P1), real mtp_vfs.c + mtp_store_pfs.c on fake PFS, mock TinyUSB endpoints, libmtp-style scripted session.
  15 groups, 8427 checks, 0 failed (ASan/UBSan). First run on unfixed code: 758 failed checks = 11 real defects, all fixed (marked `FORK (MTP-H)`, Deviation Log rows).
* Fixed: refused SendObjectInfo/SendObject deadlock (data phase now read and dropped, then error 0x200E/0x200F/0x2005); response mid-transfer went out as file bytes; SessionNotOpen / OpenSession id 0 / SessionAlreadyOpen;
  GetObjectHandles root(0xFFFFFFFF) vs all(0) swapped; root ParentObject 0; ThumbFormat, ObjectInfo dates, ProtectionStatus=1, ObjectFileName Get-only, GetObjectPropList handle 0, DeviceInfo events.
* Not fixed (documented): transaction IDs unchecked; GetObjectPropList(folder, depth 1) includes the folder; CLEAR_HALT alone does not revive bulk pipe (0x66 does); DEVICE_RESET closes the session.
* Cannot show: real libmtp/Windows/macOS, DWC2/DMA, USB task timing/stack (6144 B estimate), real SD, usb_files.c/boot code. Highest-risk change on a real host: GetObjectHandles root fix.
* daq_mtp_force builds clean; nm shows T for 6 tud_mtp_*_cb and mtp_dispatch.
* Window B agent tokens so far: ~450k (window A total ~2.6M).

## 07:0x OPUS REVIEW of the USB boot code (~190k tokens, commit fcbb2d1; Review_UsbFiles.md MTP-V-1..13). Verdict: daq_mtp_force safe to flash.
* Every boot path keeps USB-Serial-JTAG alive until TinyUSB starts; a full power cycle always gives the 10 s grace.
* Fixed/changed (marked `REVIEW (MTP-V)`, Deviation Log rows): SD store race between cmd task and USB task; refresh freed fd slots; TinyUSB task stack 6144 -> 8192; LOG ends with a DIAG line (heap, stack); failed CDC init drops D+ and restarts.
  Builds clean, nm shows T for tud_mtp_*_cb, host test 8427/8427.
* OPEN RISKS: V-1 (HIGH): after panic/WDT reset in files mode D+ stays up -> stale 4041 device; probably recovers ~10 s later (HANG test must confirm); crash hook fix is written in MTP-V-1.3, NOT applied (needs hardware proof).
  V-3: OpenSession refresh blocks the USB task up to 10 s (30 s max): EP0/CDC dead meanwhile. V-4: stacks unmeasured (read the DIAG line). V-12: HANG ships in the image.
* **HARD RULE (MTP-V-2): do NOT test `LEAVE`.** LEAVE boots daq_mtp SERIAL mode: read-write PFS mount, can erase NAND, starts pfs_writer, SD logger, WiFi.
  The hardware agent must SKIP step MTP-M-7.5 (LEAVE). Use REBOOT (back to files mode) and DL only. Only test LEAVE with the user's permission.
* Windows: no MS OS/BOS descriptor needed (unverified on a real PC). macOS: ptpcamerad grabs the interface; stop it before OpenMTP.
* Window B agent tokens so far: ~0.85M (host harness 386k, opus review 190k + orchestrator).

## 07:37 BOARD BACK (user reconnected): 303a:4041 'IREC DAQ', /dev/ttyACM0 (a TinyUSB firmware is running, probably spike v1/v3). Started sonnet HARDWARE agent (Hardware_Report.md): INFO, flash daq_mtp_force via DL, grace proof, REBOOT, DL, optional HANG, ptp_probe 1-6 on real NAND, then libmtp. LEAVE forbidden.

## 07:50 HARDWARE agent stopped at step 2 (~60k tokens, commit fbe2920; Hardware_Report.md MTP-HW-1..3)
* Board was running the OLD spike (INFO: "spike files mode"). CDC `DL` on it dropped the board again (no 303a). My plan was wrong: DL on spike v1/v3 is NOT safe (known from the FIX agent report). 
* daq_mtp_force is built and ready (host test 8427/8427, nm T for the 6 callbacks, RAM 64.7 %, flash 27.7 %). Nothing flashed.
* NEEDS HUMAN: unplug 3 s; hold BOOT, plug in; `lsusb` shows 303a:1001; flash:
  `cd .claude/worktrees/mtp && ~/.platformio/penv/bin/pio run -e daq_mtp_force -t upload --upload-port /dev/ttyACM0`; unplug 3 s, plug in WITHOUT BOOT.
  Then restart the hardware agent at step 3 (power-cycle proof, INFO/LOG DIAG, REBOOT, DL reflash, optional HANG, ptp_probe, libmtp).
* Rule: never send DL to the old spike firmware. First daq_mtp_force flash needs BOOT.

## 08:25 HARDWARE (orchestrator, inline, with the user's BOOT flash): daq_mtp_force FLASHED and WORKS so far
* BOOT flash ok (watcher script; note ttyACM0 appears ~1 s after 303a:1001). Power-cycle proof PASS: 303a:1001 for ~10 s then 303a:4041. INFO: mode=files mounted=1 blocked_writes=0 nor_blocked=0 lost_pages=0, rec=0.
  LOG: NAND mounted read-only (Path A), MTP storage id 0x00010001 name=DAQ total=122945536, DIAG heap_free=56904 min=53624, stack free TinyUSB=6804 mtp_vfs=7500 cmd=6560 main=1456 led=2120.
* REBOOT PASS (clean 1001 -> 4041, no replug). DL PASS (1001 in ~1 s), reflash with no BOOT PASS, boots to files mode. The way back with no button is proven.
* Board firmware now: daq_mtp_force. Next: ptp_probe stages 1-6 on the real NAND, mtp-detect last. LEAVE still forbidden. HANG test not yet done.

## 08:35 RESULT: MTP WORKS ON THE BOARD (Linux). See Hardware_Report.md MTP-HW-4/5.
* daq_mtp_force on the board. ptp_probe 1-6 PASS, libmtp mtp-detect/folders/files/getfile PASS, real file CRC matches, random reads OK, 767 KB/s, writes refused. The old spike failure was the unlinked tud_mtp_*_cb (fixed in the merged fork).
* NAND: user approved a 30 s daq_sim recording: 173 pages (~2.8 Mbit, 0.6 % of 500 Mbit). The NAND now holds ONE synthetic file (id 2). All other NAND use: reads.
* NOT tested: HANG (crash recovery, risk V-1), SD card (none), Windows, macOS, desktop file manager, long transfers, MATLAB over CDC while MTP runs. LEAVE forbidden.
* Next (optional): HANG test with the user present; gvfs/file-manager test; stress read (full file 20x); Windows/macOS need other machines.

## 09:0x Soak/concurrency agent (~120k tokens, commit d532821; Hardware_Report.md MTP-HW-6)
* 30/30 probe stage-5 runs PASS, 5/5 mtp-getfile CRC f3d49e4f, 714-718 KB/s, INFO clean (blocked_writes=0) after each. 225/225 CDC INFO answered during MTP reads, no speed change.
* DIAG: heap_free 56692 min 49524; stack free TinyUSB 5540, mtp_vfs 5884, cmd 6304, main 1376 (lowest), led 2120. Earlier "missing DIAG" was a short reader: read until END.
* File manager (KDE/gvfs) not testable: gvfs-mtp/jmtpfs not installed; Solid shows "Camera" (udev ID_MTP_DEVICE missing); a replug may fix. Not a firmware proof either way.
* Open: file manager, Windows, macOS, HANG (crash recovery), SD card, lower the MTP trace level for release (log ring fills).
* Window B agent tokens so far: ~1.9M (host 386k, opus 225k, hw 65k, soak 120k, + orchestrator).

## 09:0x Polish + user guide (~110k tokens; commits 333c862, 935414a)
* Main stack +2 KB (5632 B), tinyusb_mtp_* log tags at WARN, HANG only in env daq_mtp_force (USB_FILES_TEST_CMDS; closes MTP-V-12). daq_mtp_force/daq_mtp/daq build clean, nm T x6 in both mtp envs, host test 8427/8427. NOT flashed: the board still runs the pre-polish build.
* Docs/MTP/User_Guide.md (99 lines). NAND file names are NAND_F<5 decimal digits>D<device>.BIN (not hex). UNVERIFIED: MATLAB apps find the board in files mode (they look for "DAQBOARD V1" on PING), LED behaviour.
* Next: flash the polished daq_mtp_force via DL (proven path), re-run probe 1-6 and read DIAG main=.
* Window B agent tokens so far: ~2.0M.

## KDE file-manager test and polish verification (MTP-HW-7, small hw agent)
* Polished daq_mtp_force is on the board (DL flash OK, DIAG main stack free 3408). KDE path via `kioclient5` works: `mtp:/IREC DAQ/DAQ/NAND`, 3 copies CRC f3d49e4f at about 0.6 s each; write and delete refused; INFO clean (blocked_writes=0 nor_blocked=0 lost_pages=0). Raw libusb tools are blocked by kiod6 (host side). Open: Windows, macOS, HANG, SD card. Details: Hardware_Report.md MTP-HW-7.

## 09:3x KDE test PASS (kioclient5, 3 copies CRC ok ~540 KB/s, writes refused), Final_Summary.md written (974f884). Fixed the stale Deviation Log row. Board: polished daq_mtp_force (code 333c862).
* Corrections to the summary: probe runs = 30 soak + 6 concurrency (36) + 10 earlier; this board has NO MODE2 switch (GPIO2 high): use env daq_mtp_force (User_Guide says "switch": needs a note for boards with the switch).
* Remaining: HANG (needs user near the board), SD card, Windows, macOS. Heartbeats: no work unless the user asks. Final summary job at 10:51.

## 09:3x HANG crash-recovery test (MTP-HW-8): PASS, 5 of 5 cycles, no human
* `HANG` (task watchdog) 3 times idle, 2 times during a `kioclient5` read. Host saw a disconnect at 8.7 s, 1001 at 9.1 s (11 s grace), 4041 at 20.5 s. No stale device (MTP-V-1 not reproduced on this PC), no xhci error, other USB devices unaffected. LOG shows `reset reason 6` (task WDT) and the grace countdown. INFO clean after each cycle (blocked_writes=0 nor_blocked=0 lost_pages=0 mounted=1). Reads after recovery CRC f3d49e4f. The read in progress fails after about 8.7 s and leaves a partial file. CDC `DL` + pio flash still work after the crashes (reflashed the same daq_mtp_force). Fix MTP-V-1.3 not needed on this evidence, not applied. Not tested: panic/abort/INT-WDT crashes, Windows/macOS, crash in `daq` serial mode. Details: Hardware_Report.md MTP-HW-8.

## MTP-HW-9 (11 Nov, user at the PC): switch test step 1 PASS. See Hardware_Report.md MTP-HW-9.
* Env `daq_mtp` (no force) is on the board, MODE2 switch in FILES position (GPIO 2 low): 1001 for the grace window, then 4041. INFO mode=files blocked_writes=0 nor_blocked=0 lost_pages=0. 3 kioclient5 copies CRC f3d49e4f. `HANG` is refused in this image (string absent). nm T x6, host test 8427/8427. NAND writes: zero.
* Next: the user flips MODE2 to the serial position and power cycles; check list in MTP-HW-9.4 (only 1001, no 4041, DAQ banner, INFO mode=serial, rec locked, no TinyUSB, no WDT reboot). No LEAVE, no REC ON.

## MTP-HW-10 (12 Nov, user at the PC): a DIFFERENT board (serial 28:84:85:46:83:04), see Hardware_Report.md MTP-HW-10.
* Found plain `daq` (fa2d136-dirty, Oct 30), rec=0. Flashed `daq_mtp` (358a709) with a normal pio upload (hash ok). MODE2 low: 1001 about 6 s, then 4041, stable. INFO mode=files blocked_writes=0 nor_blocked=0 lost_pages=0. PING gives `DAQBOARD V1`; `MATLAB PING` is refused in files mode. This NAND is EMPTY (LIST 0 files), so no copy/CRC test. Rule: the TinyUSB CDC needs DTR=1, else no reply. NAND writes: zero.
* Next: flip MODE2 high, power cycle, run MTP-HW-9.4 (serial position).
