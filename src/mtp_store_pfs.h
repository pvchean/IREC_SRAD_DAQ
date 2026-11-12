/* PFS (NAND) backend of the MTP files mode, and the read-only mount of PFS. See Docs/MTP/Storage_Design.md (MTP-S-3, MTP-S-4). */
#ifndef MTP_STORE_PFS_H
#define MTP_STORE_PFS_H

#include <stdbool.h>
#include <stdint.h>

#include "pfs.h"
#include "mtp_store.h"

/* Mount PFS so that no byte can reach the NAND or the NOR store. It wraps the two HALs: the NAND HAL passes the calls
   init, read_page, and is_bad_block to the real driver, and it refuses write_page, erase_block, and mark_bad_block. The NOR HAL
   passes mount and read, and it refuses write_atomic. Each refused call is counted (mtp_pfs_blocked_writes()).
   Use it in place of pfs_mount() in files mode. Files mode only: it returns PFS_ERR_INVALID_ARG in serial mode.
   The caller must not start pfs_writer_run() and must not call pfs_ingest() or pfs_flush(). It must start pfs_scan_run(). */
pfs_err_t mtp_pfs_mount_readonly(pfs_ctx_t *ctx, const nand_hal_t *nand, const pfs_nor_hal_t *nor);

/* Number of write, erase, and mark-bad calls that the shim refused. NAND and NOR. In a correct run it is 0 or small; any value
   above 0 shows that PFS wanted to write at the mount (an interrupted erase, stale blocks): see Storage_Design.md MTP-S-2.4. */
uint32_t mtp_pfs_blocked_writes(void);
uint32_t mtp_pfs_blocked_nor_writes(void);

/* Bind the backend to the mounted context. false in serial mode, or if ctx is NULL. Then mtp_store_pfs_get() gives the vtable. */
bool mtp_store_pfs_init(pfs_ctx_t *ctx);

/* Pages that could not be read and were given as 0xFF filler, since init. */
uint32_t mtp_store_pfs_lost_pages(void);

#endif
