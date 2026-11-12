/* Host test helpers for src/mtp_store_pfs.c (Claude helper, not a deliverable). */
#ifndef MTP_HOST_FAKE_H
#define MTP_HOST_FAKE_H
#include <stdio.h>
#define mtp_files_mode() 1
#define mtp_addr()       3
#define mtp_delay_ms(ms) ((void)(ms))
#define MTP_LOG(...)     do { printf("  log: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#endif
