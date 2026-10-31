/* Worst-case backup: the same 8-byte records as the PFS file (both sensors, see record.h), on a FAT micro SD card (SDMMC, 1-bit).
   The file name is F<PFS file ID, 5 digits>D<device ID 0..7>.BIN (8.3 name), so an SD file and a PFS file with the same ID hold the
   same sensor stream. File content: no header, no footer. A plain series of 8-byte records, little endian. See
   record.h. The card can be missing: then the samples are dropped and the SD LED is off. */
#ifndef SD_LOG_H
#define SD_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pfs.h"
#include "record.h"

void sd_log_start(pfs_ctx_t *pfs);                 /* call after pfs_mount() */
void sd_log_push(const record_t *r);               /* own record, never waits; one writer at a time */
/* Records of another board (sensor types 1 and 2): saved to F<own PFS file ID>D<remote ID>.BIN. Never waits. */
void sd_log_push_remote(uint8_t dev, const uint8_t *records, size_t len);
bool sd_log_mounted(void);
bool sd_log_flush(uint32_t timeout_ms);            /* write all staged bytes now and close the files (they open again with the next data); false if no card or error */
uint32_t sd_log_dropped(void);                     /* records lost before the SD task took them */
uint32_t sd_log_errors(void);                      /* failed writes */
uint64_t sd_log_bytes(void);

#endif
