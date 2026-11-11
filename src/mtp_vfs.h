/* Read-only VFS shim of the MTP files mode (Docs/MTP/Merge_Report.md MTP-M-3).
   It makes ONE VFS tree at /daq from the two storage backends of mtp_store.h:
     /daq/NAND/NAND_F<id>D<dev>.BIN   (PFS files)
     /daq/SD/uSD_<name>               (files of the micro SD card)
   The esp_tinyusb MTP layer registers /daq as its only storage. All write calls fail with EROFS.
   The file list is a snapshot. mtp_vfs_refresh() makes it again; the MTP layer calls it at each OpenSession (hook in the fork), never at boot.
   No card = the folder SD is empty. A store that is not bound (NULL) = an empty folder. */
#ifndef MTP_VFS_H
#define MTP_VFS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MTP_VFS_BASE "/daq"

/* Register the VFS. Call after mtp_store_pfs_init() and mtp_store_sd_init(). It does not read any file list. */
esp_err_t mtp_vfs_init(void);

/* Rebuild the file lists. It runs the store refresh() calls in a helper task and waits (it feeds the task watchdog of the
   calling USB task while it waits, 30 s at most). Open file handles of the old list become invalid. */
void mtp_vfs_refresh(void);

/* Total size that the MTP StorageInfo reports (the NAND capacity; the free space is 0). */
uint64_t mtp_vfs_capacity(void);

/* true if the SD card is mounted (it can try a mount). Takes the shim lock, so it never runs at the same time as an MTP read or a
   refresh of the SD store. false before mtp_vfs_init(). For the cmd task (SDLS, CHUNK SD) in files mode. */
bool mtp_vfs_sd_present(void);

#endif
