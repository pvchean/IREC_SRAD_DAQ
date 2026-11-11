/* NOR store HAL for the files mode: LittleFS in the partition pfs_nor, mounted READ ONLY, and never formatted.
   The stock HAL (pfs_nor_littlefs_get) mounts with format_if_mount_failed = 1, so a damaged store would be erased.
   Same interface and same record files as the stock HAL, so a store that the DAQ wrote reads back. write_atomic refuses. */
#ifndef NOR_RO_H
#define NOR_RO_H

#include <stdint.h>

#include "pfs.h"
#include "pfs_esp32s3.h"

const pfs_nor_hal_t *nor_ro_littlefs_get(const pfs_nor_littlefs_cfg_t *cfg);
uint32_t nor_ro_refused_writes(void);

#endif
