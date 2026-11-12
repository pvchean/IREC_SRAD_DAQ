/* PFS (NAND) backend of the MTP files mode. Read only. Docs/MTP/Storage_Design.md MTP-S-3 (API), MTP-S-4 (file names), MTP-S-2 (mount).
   One PFS file = one MTP file, from the first whole 8-byte record to the end (as the FILES command does).
   The file offset 0 is the PFS offset `start`; the file size is the PFS size minus start. */
#include <stdio.h>
#include <string.h>

#include "pfs_zd35q1gc.h"           /* NAND_PAGE_SIZE and the geometry */

#include "mtp_store_pfs.h"

#ifdef MTP_HOST_TEST
#include "mtp_host_fake.h"          /* the test gives: mtp_files_mode(), mtp_addr(), mtp_delay_ms(), MTP_LOG */
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io.h"
#define mtp_files_mode() io_usb_files_mode()
#define mtp_addr()       io_addr()
#define mtp_delay_ms(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#define MTP_LOG(...)     ESP_LOGW("mtp_pfs", __VA_ARGS__)
#endif

/* The payload of one PFS page: the same formula as pfs_internal.h (LL-V4-3.2) with NAND_PAGE_SIZE.
   9 codewords of 211 data bytes = 1899 protected bytes, minus 23 bytes of header, footer, and CRC = 1876. */
#define RS_K      ((NAND_PAGE_SIZE + 254U) / 255U)
#define CW_DATA   ((NAND_PAGE_SIZE - RS_K * 16U) / RS_K)
#define PAYLOAD   (RS_K * CW_DATA - 23U)
_Static_assert(PAYLOAD == 1876U, "PFS page payload changed: check pfs_internal.h PFS_PAYLOAD_SIZE");

#define MAX_FILES     128U          /* PFS_MAX_TRACKED_FILES */
#define INDEX_WAIT_MS 10000U        /* the background scan needs about 2 s at the 80 MHz clock (README) */
#define FILL          0xFFU         /* the value of a part that cannot be read: an unknown META record, ignored by the readers */

typedef struct { uint16_t id; uint64_t start; uint64_t size; bool exact; } ent_t;

static pfs_ctx_t *s_pfs;
static bool       s_ready;
static ent_t      s_ent[MAX_FILES];
static uint32_t   s_n;
static uint32_t   s_lost_pages;

/* one-page cache: a read of 512 bytes would else read the same NAND page 4 times */
static uint8_t    s_pg[PAYLOAD];
static int32_t    s_pg_ent = -1;
static uint64_t   s_pg_idx;

/* ---- read-only mount (MTP-S-2) ---- */
static const nand_hal_t     *s_real_nand;
static const pfs_nor_hal_t  *s_real_nor;
static volatile uint32_t     s_blocked, s_blocked_nor;

static nand_status_t ro_init(void)                                             { return s_real_nand->init(); }
static nand_status_t ro_read(uint16_t b, uint16_t p, uint8_t *m, uint8_t *o, nand_ecc_t *e) { return s_real_nand->read_page(b, p, m, o, e); }
static nand_status_t ro_bad(uint16_t b, bool *bad)                             { return s_real_nand->is_bad_block(b, bad); }
static nand_status_t ro_write(uint16_t b, uint16_t p, const uint8_t *m, const uint8_t *o, nand_ecc_t *e)
{ (void)b; (void)p; (void)m; (void)o; (void)e; s_blocked++; return NAND_ERR_PROTECTED; }
static nand_status_t ro_erase(uint16_t b)                                      { (void)b; s_blocked++; return NAND_ERR_PROTECTED; }
static nand_status_t ro_mark(uint16_t b)                                       { (void)b; s_blocked++; return NAND_ERR_PROTECTED; }
static const nand_hal_t s_ro_nand = { ro_init, ro_read, ro_write, ro_erase, ro_bad, ro_mark };

static pfs_hal_status_t ro_nor_mount(void)                                     { return s_real_nor->mount(); }
static pfs_hal_status_t ro_nor_read(const char *n, uint8_t *d, size_t m, size_t *l) { return s_real_nor->read(n, d, m, l); }
static pfs_hal_status_t ro_nor_write(const char *n, const uint8_t *s, size_t l)
{ (void)n; (void)s; (void)l; s_blocked_nor++; return PFS_HAL_ERR_IO; }
static const pfs_nor_hal_t s_ro_nor = { ro_nor_mount, ro_nor_read, ro_nor_write };

pfs_err_t mtp_pfs_mount_readonly(pfs_ctx_t *ctx, const nand_hal_t *nand, const pfs_nor_hal_t *nor)
{
    if (!mtp_files_mode() || ctx == NULL || nand == NULL || nor == NULL) { return PFS_ERR_INVALID_ARG; }
    s_real_nand = nand;
    s_real_nor  = nor;
    return pfs_mount(ctx, &s_ro_nand, &s_ro_nor);
}

uint32_t mtp_pfs_blocked_writes(void)     { return s_blocked; }
uint32_t mtp_pfs_blocked_nor_writes(void) { return s_blocked_nor; }
uint32_t mtp_store_pfs_lost_pages(void)   { return s_lost_pages; }

/* ---- the store ---- */
static bool pfs_present(void) { return s_ready; }

static int pfs_refresh(void)
{
    pfs_status_t st;
    memset(&st, 0, sizeof st);
#if defined(PFS_VERSION) && PFS_VERSION >= 400      /* PFS 0.4.0 added struct_size (v0.3.1 has no such field) */
    st.struct_size = sizeof st;
#endif
    if (!s_ready) { return -1; }
    for (uint32_t t = 0; t < INDEX_WAIT_MS; t += 50U) {          /* the file index is complete after the scan task ends */
        pfs_get_status(s_pfs, &st);
        if (st.index_ready) { break; }
        mtp_delay_ms(50U);
    }
    if (!st.index_ready) { MTP_LOG("file index not ready: the list can be short"); }
    s_n = 0;
    s_pg_ent = -1;
    uint16_t n = pfs_file_count(s_pfs);
    for (uint16_t i = 0; i < n && s_n < MAX_FILES; i++) {
        pfs_file_info_t fi, fs;
        if (pfs_file_info(s_pfs, i, &fi) != PFS_OK) { continue; }
        ent_t *e = &s_ent[s_n];
        e->id    = fi.file_id;
        e->start = (fi.first_offset + 7U) / 8U * 8U;             /* first whole record, as FILES does */
        e->size  = fi.size_bytes;
        e->exact = false;
        if (pfs_file_stat(s_pfs, fi.file_id, &fs) == PFS_OK && (fs.flags & PFS_FILE_SIZE_EXACT)) {   /* reads the last page only */
            e->size  = fs.size_bytes;
            e->exact = true;
        }
        e->size = e->size > e->start ? e->size - e->start : 0U;
        s_n++;
    }
    return (int)s_n;
}

static uint32_t pfs_count_files(void) { return s_n; }

static bool pfs_info(uint32_t i, mtp_file_info_t *out)
{
    if (i >= s_n || out == NULL) { return false; }
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "NAND_F%05uD%u.BIN", (unsigned)s_ent[i].id, (unsigned)mtp_addr());
    out->size       = s_ent[i].size;
    out->size_exact = s_ent[i].exact;
    out->mtime      = 0;
    return true;
}

/* Load page pg of PFS file e into the cache. A page that cannot be read becomes filler, so the offsets stay true. */
static void load_page(uint32_t i, uint64_t pg)
{
    size_t got = 0;
    pfs_err_t r = pfs_read(s_pfs, s_ent[i].id, pg * PAYLOAD, s_pg, PAYLOAD, &got);
    if (r != PFS_OK && r != PFS_ERR_EOF) {                       /* corrupt, trimmed, not found, not mounted */
        s_lost_pages++;
        MTP_LOG("PFS file %u page %llu: read error %d, filler used", (unsigned)s_ent[i].id, (unsigned long long)pg, (int)r);
        got = 0;
    }
    if (got < PAYLOAD) { memset(s_pg + got, FILL, PAYLOAD - got); }   /* short page, or past the end of an approximate size */
    s_pg_ent = (int32_t)i;
    s_pg_idx = pg;
}

static int32_t pfs_read_store(uint32_t i, uint64_t offset, uint8_t *buf, size_t len)
{
    if (!s_ready || i >= s_n || buf == NULL) { return -1; }
    const ent_t *e = &s_ent[i];
    if (offset >= e->size) { return 0; }
    if (len > e->size - offset) { len = (size_t)(e->size - offset); }
    if (len > 0x7FFFFFFFU) { len = 0x7FFFFFFFU; }
    size_t done = 0;
    uint64_t p = e->start + offset;                              /* the offset in the PFS stream */
    while (done < len) {
        uint64_t pg = p / PAYLOAD;
        size_t   in = (size_t)(p % PAYLOAD);
        size_t   n  = PAYLOAD - in < len - done ? PAYLOAD - in : len - done;
        if (s_pg_ent != (int32_t)i || s_pg_idx != pg) { load_page(i, pg); }
        memcpy(buf + done, s_pg + in, n);
        done += n;
        p    += n;
    }
    return (int32_t)done;
}

static uint64_t pfs_capacity(void) { return (uint64_t)NAND_BLOCKS_PER_DRIVE * NAND_PAGES_PER_BLOCK * PAYLOAD; }

static const mtp_store_t s_store = { 1U, "NAND", pfs_present, pfs_refresh, pfs_count_files, pfs_info, pfs_read_store, pfs_capacity };

bool mtp_store_pfs_init(pfs_ctx_t *ctx)
{
    if (!mtp_files_mode() || ctx == NULL) { return false; }
    s_pfs = ctx;
    s_n = 0;
    s_pg_ent = -1;
    s_ready = true;
    return true;
}

const mtp_store_t *mtp_store_pfs_get(void) { return s_ready ? &s_store : NULL; }
