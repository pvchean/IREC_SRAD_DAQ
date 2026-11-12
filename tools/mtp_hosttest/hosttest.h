/* Interface between the host test units (Claude helper, not a deliverable). */
#ifndef HOSTTEST_H
#define HOSTTEST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EP_OUT 0x02
#define EP_IN  0x82
#define EP_EVT 0x83
#define PKT    64          /* full speed bulk packet size */

/* logging (stubs.c) */
extern int hl_verbose;
extern int hl_count[5];                 /* E, W, I, D, V */
const char *hl_last_error(void);

/* VFS routing counters (stubs.c): calls from the MTP layer to the shim that could change data */
extern int h_vfs_write_calls;           /* open for write, write, unlink, rename, mkdir, rmdir, truncate */
extern int h_wdt_resets;
extern int h_vfs_opens, h_vfs_reads;

/* USB mock (stubs.c) */
void um_reset(void);                    /* clear the mock and let the real driver open its interface (mtpd_init + mtpd_open) */
int  um_pump(void);                     /* run the device side until idle; returns events handled */
int  um_host_out(const uint8_t *d, size_t n);        /* 0 ok, -1 timeout (device never armed OUT), -2 STALL */
int  um_host_in_packet(uint8_t *p, size_t *n);       /* 0 ok, -1 timeout (nothing queued), -2 STALL */
bool um_stalled(uint8_t ep);
void um_clear_halt(uint8_t ep);         /* CLEAR_FEATURE(ENDPOINT_HALT) as the host core does it */
/* class control request: dir_in 1 = device to host. Returns 0 ok, -1 stalled in SETUP, -2 stalled in ACK */
int  um_control(int dir_in, uint8_t req, uint16_t wlen, const uint8_t *out, uint8_t *in, int *in_len);
size_t um_out_pending(void);            /* bytes the host wrote that the device has not read */
bool um_in_pending(void);               /* device has queued IN data the host has not read */

/* fake stores (fake_stores.c) */
typedef struct { uint16_t id, first_seq, last_seq; uint32_t last_len; bool exact; int bad_page; } fake_pfs_file_t;
void fake_pfs_setup(void);              /* mount through mtp_pfs_mount_readonly(), bind the store */
void fake_pfs_set_files(const fake_pfs_file_t *f, int n);
void fake_pfs_set_many(int n);          /* n small files, ids 100.. */
void fake_sd_set(bool present, int n);  /* present flag and number of files (0..256) */
uint8_t fake_sd_byte(int file, uint64_t off);
void fake_sd_fail_at(int64_t off);      /* SD reads that end after this offset fail (-1 = never) */
extern int fake_nand_writes;            /* real write/erase/mark calls that reached the fake NAND (must stay 0) */
#endif
