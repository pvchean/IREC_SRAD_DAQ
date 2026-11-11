# MTP Host Test Report (the real MTP handlers on a Linux host)

Author: MTP HOST-TEST agent. Date: 2026-11-11. Branch `mtp-usb-mode`. Language: ASD-STE100. Labels: MTP-H-n.
No hardware was used. No /dev/ttyACM* was opened. Nothing was flashed. Zero NAND writes (the fake NAND counts any write: it stayed 0).

## MTP-H-1 Result in one table

| Item | State |
|---|---|
| Host test (`tools/mtp_hosttest/run.sh`) | 15 test groups, 8427 checks, 0 failed. Built with ASan and UBSan: no report. LeakSanitizer: no leak in the firmware code. |
| First run on the unfixed code | 758 failed checks. They came from 11 real defects (MTP-H-4). All 11 are fixed. |
| `pio run -e daq_mtp_force` after the fixes | SUCCESS, no warning. RAM 64.7 %, flash 27.7 % (same as before). |
| `nm -C .pio/build/daq_mtp_force/firmware.elf \| grep tud_mtp_` | `T` for `tud_mtp_command_received_cb`, `tud_mtp_data_xfer_cb`, `tud_mtp_data_complete_cb`, `tud_mtp_request_cancel_cb`, `tud_mtp_request_device_reset_cb`, `tud_mtp_request_get_device_status_cb`, and `T mtp_dispatch`. `W` (weak, unused) for `get_extended_event_cb`, `vendor_cb`, `response_complete_cb`, as before. |
| Hardware test | NOTHING tested on hardware. See MTP-H-6. |

## MTP-H-2 How to build and run (one script)

```
tools/mtp_hosttest/run.sh        # build and run; exit code 0 = all checks passed
tools/mtp_hosttest/run.sh -v     # also print the log lines of the firmware code (ESP_LOGE and ESP_LOGW)
tools/mtp_hosttest/run.sh -vv    # all log levels
```

Needs gcc with ASan and UBSan, and the PFS headers (`/home/atomic/Documents/cpp_projects/Paranoid-FS/components/pfs/include`, override with `PFS_INC`). The build goes to `$TMPDIR/mtp_hosttest_build`. The script makes `sdkconfig.h` from `sdkconfig.daq_mtp_force` (so the buffer size 4096, 512 objects, and trace flag are the real ones) and makes a copy of the real `tusb_config.h` with only the RTOS line changed (`OPT_OS_NONE`). The run takes about 20 s.

What is compiled (the REAL sources, no copy, no edit):
* `mtp_usb/fork/esp_tinyusb/tinyusb/src/class/mtp/mtp_device.c` (TinyUSB MTP class driver with the P1 recovery back-port),
* all nine `mtp_usb/fork/esp_tinyusb/mtp/*.c` (transport, operations, properties, read, write, transfer, object store, codec, driver),
* `src/mtp_vfs.c` (the read-only VFS shim, with its refresh task) and `src/mtp_store_pfs.c` (the PFS store with the read-only mount).

What is replaced (files in `tools/mtp_hosttest/`):
* `stubs.c`: FreeRTOS on pthreads (a mutex taken twice by one task aborts the test: that is a deadlock on the board), `esp_log`, `esp_task_wdt`, the VFS table of `esp_vfs_register()`, and the POSIX calls of the fork (`fopen`, `fread`, `stat`, `opendir`, ... go to `mtp_vfs.c` through that table, as the IDF VFS layer does), and the **TinyUSB device layer**.
* The USB mock sits under the real `mtp_device.c`. It implements `usbd_edpt_xfer`, `claim`, `busy`, `stall`, `clear_stall`, `tud_control_xfer`, `tud_control_status`. A transfer completes only when the host moves the data (OUT: host wrote the packets; IN: host read all packets). Completion is an event; `um_pump()` runs the events in order like the TinyUSB task. Full speed, 64 byte packets, a transfer ends at the full length or at a short packet or a ZLP. Endpoint buffers are used live (no copy), so a buffer that is reused too early gives wrong bytes. A stall sets "stalled and busy", the clear resets both (TinyUSB behaviour).
* `fake_stores.c`: the fake PFS and fake NAND of `tools/mtp_store_test.c` (7 files, see MTP-H-3), and an in-memory SD store (the real SD store needs the SDMMC driver). The fake NAND counts every write, erase, and mark-bad call that reaches it.
* `main.c`: the host side. It acts like libmtp: command, then (for host data) the data container at once, then it reads containers until the response. A write that the device does not read is a timeout (the host would hang). It checks the USB framing of every container (length, ZLP rule) and parses every dataset to the last byte.

## MTP-H-3 What the test covers (all pass)

| Test | Content |
|---|---|
| T01 | Commands before OpenSession: GetDeviceInfo works; GetStorageIDs, GetObjectHandles, GetObjectInfo, CloseSession give SessionNotOpen. |
| T02 | GetDeviceInfo dataset field by field: versions, extension string (count byte), operation list (18 read operations, no write operation), events, device properties, formats, four strings, exact length. |
| T03 | OpenSession (id 0 refused, id 1 OK, second one gives SessionAlreadyOpen with the id, refresh hook runs once per real open), CloseSession, command after close, reopen. |
| T04 | GetStorageIDs, GetStorageInfo: type 3, hierarchical, **AccessCapability 1 (read-only)**, capacity, free space 0, description "DAQ", wrong ids. |
| T05 | GetObjectHandles: root gives exactly the folders NAND and SD; NAND and SD folders give the store files; format filter; all storages; parent 0 gives all 12 objects; GetNumObjects; wrong storage, parent, handle. GetObjectInfo of 2 folders and 10 files: every field (storage, format, **ProtectionStatus 1**, size, thumb fields 0, parent, association type 1 for folders, name in UTF-16, dates). |
| T06 | GetObject of all 10 files compared byte for byte with the store (7 NAND files incl. empty, 1 byte, 3776 = 59 * 64, 4084 (container 4096), 4096, a file with an unreadable page (0xFF filler), a 73 260 byte trimmed file; 3 SD files). ZLP rule on every container. A PFS generator spot check (trim start 1880). Folder and bad handle. |
| T07 | GetPartialObject and GetPartialObject64 (0x95C1): 22 offsets x 13 lengths on the trimmed file (page borders 1875 to 1878, 3751 to 3753, EP buffer borders 4083 to 4097, 8179 to 8181), end of file, length beyond the end (clipped), offset = size (empty), offset > size (error), length 0 and 0xFFFFFFFF, offsets above 4 GB, the unreadable page, empty file, 120 seeded random reads per operation. ActualBytes response parameter checked every time. |
| T08 | 19 write operations (SendObjectInfo, SendObject, DeleteObject file/folder/all, FormatStore, SetObjectProtection, MoveObject, CopyObject, SetObjectPropValue, SendObjectPropList, SetObjectReferences, SetDevicePropValue, the five Android edit operations) each get 0x200E, 0x200F, 0x200D or 0x2005. Before and after: hash of all store content equal; 0 NAND writes; 0 write calls on the VFS shim. A SendObject with 9000 bytes of data is read and refused. The canary (below) passes after every one. |
| T09 | Unknown opcodes, a skipped, a repeated, and a zero transaction id (the device accepts and echoes the id), 50 back-to-back commands, 5 parameters, GetDeviceStatus, DEVICE_RESET while idle (closes the session), GetExtendedEventData (control pipe stalls). |
| T10 | Bad containers: data type as command, 8 byte container, stray ZLP, response type as command: both bulk endpoints stall; the class reset 0x66 recovers. CLEAR_HALT alone does NOT recover (MTP-H-5.3). |
| T11 | CANCEL (0x64) in the middle of a GetObject: the device finishes the packet and sends response 0x201F. DEVICE_RESET in the middle of a read. |
| T12 | Object properties: GetObjectPropsSupported, GetObjectPropDesc (14 properties, type, Get only), GetObjectPropValue (14 properties x 10 files), GetObjectPropList (folder depth 1, one property, root, all objects, group code, depth 0), GetObjectReferences, GetDevicePropDesc/Value (friendly name), unsupported property codes. |
| T13 | SD card removed and inserted, NAND files change, 100 close/open cycles, old handles after a refresh: the list changes only at OpenSession; stale handles give InvalidObjectHandle. |
| T14 | Maximum list: 128 NAND files + 256 SD files + 2 folders = 386 of 512 table entries. All handles, all infos, one GetObjectPropList of all 386 objects x 14 properties (about 43 KB), reopen with a full table. |
| T15 | A store read error at offset 0, 3000, 20 000, 150 000 in GetObject and GetPartialObject: the device answers 0x2002 (before the data, or in the middle of the data) and the next command works. |

The canary (after every error, in every test): GetStorageIDs must give OK and the right data, no endpoint is stalled, no data is left in the pipes. After a class reset the canary opens the session again first.

## MTP-H-4 Bugs found and fixed (11)

All changes are in the fork copy `mtp_usb/fork/esp_tinyusb` and are marked `FORK (MTP-H)` in the source. Each has a row in `Deviation Log.md`. The test fails on the old code and passes on the new code (checked for the first and the last item by removing the fix).

| # | Defect (what the old code did) | Fix | Place |
|---|---|---|---|
| 1 | **Deadlock on a refused write.** A write operation was answered in the command phase and the device never armed the OUT endpoint. A host that writes the data container before it reads the response (libmtp) waits until its timeout; the device waits for the host to read the response. | The device arms the OUT read, reads and drops the data phase, and sends the refusal (0x200E, 0x200F, 0x2005) in `tud_mtp_data_complete_cb`. Covers SendObjectInfo, SendObject, SetObjectPropValue, AndroidSendPartialObject and the not advertised SetDevicePropValue, SetObjectPropList, SendObjectPropList, SetObjectReferences. | `mtp/tinyusb_mtp_transport.c:211-236` (table), `:240-259` (dispatch), `:344` and `:405-410` (complete) |
| 2 | The class driver took the length of an OUT data phase from the app (header only, 12 bytes) and ended the phase after the first packet. | The length of the first OUT packet header is used. | `tinyusb/src/class/mtp/mtp_device.c:452-459` |
| 3 | **A response sent in the middle of a data phase was file bytes.** In a continuation packet the header is not in the endpoint buffer, but `tud_mtp_response_send()` sends the buffer. A store error (GetObject, GetPartialObject) or a CANCEL in the middle of a read sent 12 or more bytes of file data as the "response". The host never got a response and the layer was out of step. | A header-only response is built at the start of the endpoint buffer. | `mtp/tinyusb_mtp_transport.c:381-392` (new header at `:385`) |
| 4 | No session check. Every operation worked without OpenSession, and CloseSession did not stop the others. | SessionNotOpen (0x2003) for every operation except GetDeviceInfo and OpenSession (command phase). | `mtp/tinyusb_mtp_transport.c:279-285` |
| 5 | OpenSession with SessionID 0 was accepted and ran the refresh. SessionAlreadyOpen had no response parameter. | InvalidParameter (0x201D) for id 0 (before the hook). The response carries the open SessionID. | `mtp/tinyusb_mtp_operations.c:30,36-61` |
| 6 | **GetObjectHandles had the two special parents swapped.** Parent 0xFFFFFFFF (root; what libmtp sends) returned the whole tree (12 objects instead of 2); parent 0 returned the root only. | 0xFFFFFFFF = root objects only; 0 = all objects at any depth (PIMA 15740 10.5.2). Also changes GetNumObjects. | `mtp/tinyusb_mtp_operations.c:142-149` |
| 7 | ParentObject of a root object was 0xFFFFFFFF in ObjectInfo, in the property DC0B, and in the property list. Root is 0. | Output 0. | `mtp/tinyusb_mtp_operations.c:276`, `mtp/tinyusb_mtp_properties.c:159,250` |
| 8 | ObjectInfo ThumbFormat was 0x3000 (a format code, not "no thumbnail"). ObjectInfo DateCreated and DateModified were empty. | ThumbFormat 0. Dates from the object time (same format as the property list). | `mtp/tinyusb_mtp_operations.c:263,269,284-286` |
| 9 | ProtectionStatus: ObjectInfo said 1 (read-only), the property DC03 (value and list) said 0. ObjectFileName was reported "Get/Set" in GetObjectPropDesc on a read-only device. | Property is 1. ObjectFileName is Get only. | `mtp/tinyusb_mtp_properties.c:35,181,240` |
| 10 | GetObjectPropList refused handle 0. | Handle 0 = the root: with depth 1 the objects in the root (MTP 1.1). Depth 0 gives an empty list. A command with no parameter gives InvalidParameter. | `mtp/tinyusb_mtp_properties.c:389-405` and the parameter check at `:429` |
| 11 | DeviceInfo listed one supported event with the invalid code 0x0000. The empty macro made `TU_ARGS_NUM()` equal 1. | The list is `MTP_EVENT_CANCEL_TRANSACTION` (0x4001; the device honours it). | `include/tusb_config.h:187-190` |

## MTP-H-5 Findings that are NOT fixed (decisions)

1. **Transaction ids are not checked** (a skipped, repeated, or zero id gets the normal answer, the response echoes the id). PIMA 15740 says InvalidTransactionID. Not fixed on purpose: a host that restarts the numbering (after a reset) would break. libmtp and Windows number in order.
2. **GetObjectPropList(folder, depth 1) returns the folder and its children** (upstream behaviour). The MTP 1.1 text can be read both ways. Not verified against Windows. If Windows shows each folder twice, remove the folder itself from `mtp_builder_append_prop_visit_queue_locked()`.
3. **CLEAR_HALT alone does not recover the bulk pipe after a protocol error.** The driver is in the ERROR phase and only the class reset (0x66) arms the OUT read again. Windows sends 0x66. libmtp: unknown. The error phase starts only on a bad container type, a short command container, or a failed endpoint call: rare. GetDeviceStatus always says "OK, no halted endpoint".
4. **DEVICE_RESET (0x66) closes the session.** The host must open it again. The test expects this.
5. **ObjectInfo of a file larger than 4 GB gives ObjectTooLarge (0xA809).** Not a problem here: NAND is at most 123 MB, FAT32 files are below 4 GB.
6. A CANCEL in the middle of a read ends the data phase at the end of the current 4096 byte buffer, then sends 0x201F. A host that waits for the full data length sees a short container. This is the same as upstream and is the only simple way. Real hosts: untested.
7. The stale fork copy in `tools/mtp_spike/fork` does not have these fixes (and not the link anchor). Do not use it.
8. Stack: the largest frames (host numbers, x86-64, -O1) are GetObjectPropValue 1856 bytes, GetObjectInfo 1120, SendObjectInfo 1072, `mtpd_xfer_cb` 272. The worst call chain is about 3 KB plus the log call. The USB task has 6144 bytes. It looks enough but it is not measured on the chip. Check `uxTaskGetStackHighWaterMark()` of the USB task during the first hardware read.

## MTP-H-6 What a host test cannot show

* **Real hosts.** libmtp, Windows (WPD), macOS, Android File Transfer: each has its own order of commands, its own idea of what is optional, and its own recovery after an error. The test host follows the PIMA text and the libmtp style only. The fix for GetObjectHandles (MTP-H-4 #6) and the folder depth (MTP-H-5.2) are the two points where the hosts may differ.
* **The USB controller.** DWC2 FIFO sizes, DMA and cache, endpoint buffer alignment, the real timing of NAK, the data toggle after a stall, the 5 IN endpoint limit, a transfer that the controller cancels at a reset (the mock drops it at CLEAR_HALT), and a host that does not send the ZLP.
* **Tasks and time.** The real USB task runs `tud_task` with interrupts; the test runs the events in one thread. Priorities, the 8 s task watchdog, the 30 s refresh wait, and the stack use are not tested. The refresh helper task runs as a real thread, but the fake stores answer at once.
* **The real stores.** The PFS code runs on a fake PFS (the real PFS and the NAND timing are not used). The SD store is replaced by an in-memory store; `mtp_store_sd.c` (SDMMC, mount) is not compiled. Newlib `fread` buffering (the real chunk sizes of the reads) is not modelled: the test reads straight through `read_p`.
* **The rest of the firmware.** `usb_files.c` (grace window, CDC, way back, log ring), `files_mode.c`, the boot, the pad safety, and the USB descriptors (`TUD_MTP_DESCRIPTOR` is the stock TinyUSB macro: class 6, subclass 1, protocol 1, three endpoints) are not part of this test.
* **Events.** The device sends no event (no ObjectAdded and so on). The interrupt endpoint is not exercised.

## MTP-H-7 Next steps

1. Hardware order stays as in `Merge_Report.md` MTP-M-7. Run `tools/mtp_spike/host/ptp_probe.c` stage 3 against the new code: root must list 2 folders, NAND must list the files, all ParentObject values of root objects are 0.
2. Before `mtp-detect`: run `tools/mtp_hosttest/run.sh` again after every change to `mtp_usb/` or `src/mtp_vfs.c`. It is cheap (20 s) and it found 11 defects.
3. If a real host shows a different behaviour than this test, add its command order to `main.c` as a new test first, then fix.

Approximate tokens of this agent: 450k.
