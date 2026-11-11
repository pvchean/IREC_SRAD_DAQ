/* MTP spike: read-only fake VFS. Root /daq with folders NAND and SD. No storage: the bytes are made on the fly. */
#pragma once
#include <stdint.h>
#include "esp_err.h"

#define FAKE_BASE      "/daq"
#define FAKE_NAND_SIZE (64ULL * 1024 * 1024)   /* reported capacity of the storage */

/* Register the VFS at FAKE_BASE. */
esp_err_t fake_vfs_init(void);
/* Pattern of the files: byte = (offset & 0xFF) ^ file number. Used by the host test. */
