/* Host test helpers for src/mtp_store_pfs.c in the full host test (Claude helper, not a deliverable). */
#ifndef MTP_HOST_FAKE_H
#define MTP_HOST_FAKE_H
#include "esp_log.h"
#define mtp_files_mode() 1
#define mtp_addr()       3
#define mtp_delay_ms(ms) ((void)(ms))
#define MTP_LOG(...)     ESP_LOGW("mtp_pfs", __VA_ARGS__)
#endif
