/* Worst-case backup: the same 8-byte records as the PFS file, on a FAT micro SD card (SDMMC, 1-bit).
   The file name is F<PFS file ID, 5 digits>D<device ID 0..7>.BIN (8.3 name), so an SD file and a PFS file with the same ID hold the
   same sensor stream. File content: no header, no footer. A plain series of 8-byte records, little endian:
   u32 timestamp_us (ADS1115 ready edge, wraps in 71 min), i16 reading (raw ADS1115 count, 0.1875 mV per
   count), u16 seq (counts every sample, also when not recording). See record.h. The card can be missing: then the samples are dropped and the SD LED is off. */
#ifndef SD_LOG_H
#define SD_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "pfs.h"
#include "record.h"

void sd_log_start(pfs_ctx_t *pfs);                 /* call after pfs_mount() */
void sd_log_push(const record_t *r);               /* never waits */
bool sd_log_mounted(void);
uint32_t sd_log_dropped(void);                     /* records lost before the SD task took them */
uint32_t sd_log_errors(void);                      /* failed writes */
uint64_t sd_log_bytes(void);

#endif
