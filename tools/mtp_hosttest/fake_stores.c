/* Fake PFS + fake NAND (adapted from tools/mtp_store_test.c) and an in-memory SD store (Claude helper, not a deliverable).
   The real src/mtp_store_pfs.c runs on top of the fake PFS. The real SD store needs the SDMMC driver: it is replaced here. */
#include <assert.h>

#include "esp_log.h"
#include "mtp_store_pfs.h"
#include "hosttest.h"

#define PAY 1876U
#define MAXF 128
static fake_pfs_file_t F[MAXF];
static int NF;
int fake_nand_writes;

static uint8_t byte_at(int f, uint64_t o) { return (uint8_t)(o * 7U + 3U + F[f].id); }

static int find(uint16_t id) { for (int i = 0; i < NF; i++) { if (F[i].id == id) { return i; } } return -1; }

void fake_pfs_set_files(const fake_pfs_file_t *f, int n) { assert(n <= MAXF); memcpy(F, f, (size_t)n * sizeof *f); NF = n; }

void fake_pfs_set_many(int n)
{
    assert(n <= MAXF);
    for (int i = 0; i < n; i++) { F[i] = (fake_pfs_file_t){ (uint16_t)(100 + i), 0, 0, (uint32_t)(8 * (i + 1)), true, -1 }; }
    NF = n;
}

static const nand_hal_t *g_nand; static const pfs_nor_hal_t *g_nor;
pfs_err_t pfs_mount(pfs_ctx_t *c, const nand_hal_t *n, const pfs_nor_hal_t *r)
{
    (void)c; g_nand = n; g_nor = r;
    assert(n->init() == NAND_OK && r->mount() == PFS_HAL_OK);
    assert(n->erase_block(3) != NAND_OK);                      /* what PFS may try at a mount: all must be refused */
    assert(n->mark_bad_block(4) != NAND_OK);
    assert(n->write_page(1, 1, NULL, NULL, NULL) != NAND_OK);
    assert(r->write_atomic("x", (const uint8_t *)"a", 1) != PFS_HAL_OK);
    return PFS_OK;
}
void pfs_get_status(pfs_ctx_t *c, pfs_status_t *o) { (void)c; memset(o, 0, sizeof *o); o->index_ready = true; }
uint16_t pfs_file_count(pfs_ctx_t *c) { (void)c; return (uint16_t)NF; }
pfs_err_t pfs_file_info(pfs_ctx_t *c, uint16_t i, pfs_file_info_t *o)
{
    (void)c; const fake_pfs_file_t *f = &F[i]; memset(o, 0, sizeof *o);
    o->file_id = f->id; o->first_offset = (uint64_t)f->first_seq * PAY; o->size_bytes = ((uint64_t)f->last_seq + 1) * PAY;
    return PFS_OK;
}
pfs_err_t pfs_file_stat(pfs_ctx_t *c, uint16_t id, pfs_file_info_t *o)
{
    int fi = find(id); const fake_pfs_file_t *f = &F[fi];
    pfs_file_info(c, (uint16_t)fi, o);
    if (f->exact) { o->size_bytes = (uint64_t)f->last_seq * PAY + f->last_len; o->flags |= PFS_FILE_SIZE_EXACT; }
    return f->exact ? PFS_OK : PFS_ERR_CORRUPT;
}
pfs_err_t pfs_read(pfs_ctx_t *c, uint16_t id, uint64_t off, uint8_t *dst, size_t len, size_t *out)
{
    (void)c; int fi = find(id); const fake_pfs_file_t *f = &F[fi];
    *out = 0;
    assert(off % PAY == 0 && len == PAY);
    uint64_t pg = off / PAY;
    if (pg < f->first_seq) { return PFS_ERR_TRIMMED; }
    if (pg > f->last_seq) { return PFS_ERR_EOF; }
    if ((int)pg == f->bad_page) { return PFS_ERR_CORRUPT; }
    size_t n = (pg == f->last_seq && f->exact) ? f->last_len : PAY;
    for (size_t i = 0; i < n; i++) { dst[i] = byte_at(fi, off + i); }
    *out = n;
    return PFS_OK;
}
static nand_status_t t_init(void) { return NAND_OK; }
static nand_status_t t_rd(uint16_t b, uint16_t p, uint8_t *m, uint8_t *o, nand_ecc_t *e) { (void)b; (void)p; (void)m; (void)o; (void)e; return NAND_OK; }
static nand_status_t t_wr(uint16_t b, uint16_t p, const uint8_t *m, const uint8_t *o, nand_ecc_t *e) { (void)b; (void)p; (void)m; (void)o; (void)e; fake_nand_writes++; return NAND_OK; }
static nand_status_t t_er(uint16_t b) { (void)b; fake_nand_writes++; return NAND_OK; }
static nand_status_t t_bad(uint16_t b, bool *x) { (void)b; *x = false; return NAND_OK; }
static nand_status_t t_mk(uint16_t b) { (void)b; fake_nand_writes++; return NAND_OK; }
static const nand_hal_t real = { t_init, t_rd, t_wr, t_er, t_bad, t_mk };
static pfs_hal_status_t n_m(void) { return PFS_HAL_OK; }
static pfs_hal_status_t n_r(const char *a, uint8_t *d, size_t m, size_t *l) { (void)a; (void)d; (void)m; *l = 0; return PFS_HAL_OK; }
static pfs_hal_status_t n_w(const char *a, const uint8_t *s, size_t l) { (void)a; (void)s; (void)l; fake_nand_writes++; return PFS_HAL_OK; }
static const pfs_nor_hal_t rnor = { n_m, n_r, n_w };

void fake_pfs_setup(void)
{
    assert(mtp_pfs_mount_readonly((pfs_ctx_t *)1, &real, &rnor) == PFS_OK);
    assert(mtp_store_pfs_init((pfs_ctx_t *)1));
}

/* ---- in-memory SD store ---- */
static bool sd_here;
static int sd_n;
static uint32_t sd_cnt;
static const struct { const char *name; uint32_t size; } SDF[3] = { { "uSD_LOG0001.CSV", 10000 }, { "uSD_EMPTY.TXT", 0 }, { "uSD_BIG.BIN", 200000 } };

uint8_t fake_sd_byte(int file, uint64_t off) { return (uint8_t)(off * 13U + 5U + (unsigned)file * 11U); }

void fake_sd_set(bool present, int n) { sd_here = present; sd_n = n; }
static bool sd_present(void) { return sd_here; }
static int sd_refresh(void) { if (!sd_here) { sd_cnt = 0; return -1; } sd_cnt = (uint32_t)sd_n; return sd_n; }
static uint32_t sd_count(void) { return sd_cnt; }
static bool sd_info(uint32_t i, mtp_file_info_t *o)
{
    if (i >= sd_cnt) { return false; }
    memset(o, 0, sizeof *o);
    if (i < 3) { snprintf(o->name, sizeof o->name, "%s", SDF[i].name); o->size = SDF[i].size; }
    else { snprintf(o->name, sizeof o->name, "uSD_F%04u.DAT", (unsigned)i); o->size = 16; }
    o->size_exact = true;
    o->mtime = 1700000000U + i;               /* the SD card has a real time: a date must show */
    return true;
}
static int64_t sd_fail_at = -1;
void fake_sd_fail_at(int64_t off) { sd_fail_at = off; }
static int32_t sd_read(uint32_t i, uint64_t off, uint8_t *b, size_t len)
{
    if (i >= sd_cnt) { return -1; }
    if (sd_fail_at >= 0 && off + len > (uint64_t)sd_fail_at) { return -1; }     /* a hard read error (card pulled, bad sector) */
    mtp_file_info_t fi; sd_info(i, &fi);
    if (off >= fi.size) { return 0; }
    if (len > fi.size - off) { len = (size_t)(fi.size - off); }
    for (size_t k = 0; k < len; k++) { b[k] = fake_sd_byte((int)i, off + k); }
    return (int32_t)len;
}
static uint64_t sd_capacity(void) { return 0; }
static const mtp_store_t s_sd = { 2U, "SD", sd_present, sd_refresh, sd_count, sd_info, sd_read, sd_capacity };
static bool s_sd_init;
bool mtp_store_sd_init(void) { s_sd_init = true; return true; }
const mtp_store_t *mtp_store_sd_get(void) { return s_sd_init ? &s_sd : NULL; }
