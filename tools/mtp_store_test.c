/* Host test of the PFS backend of the MTP files mode, with a fake PFS and a fake NAND (Claude helper, not a deliverable).
   Build and run: tools/mtp_store_test.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mtp_store_pfs.h"

#define PAY 1876U
static int mount_calls;
static const nand_hal_t *g_nand; static const pfs_nor_hal_t *g_nor;

/* fake files: id 5 trimmed (first_seq 1), 4 pages, exact; id 7 six pages, page 2 unreadable, approximate size 1 page too large */
typedef struct { uint16_t id, first_seq, last_seq; uint32_t last_len; bool exact; int bad_page; } ff_t;
static const ff_t F[2] = { { 5, 1, 4, 100, true, -1 }, { 7, 0, 5, 1876, false, 2 } };

static uint8_t byte_at(int f, uint64_t o) { return (uint8_t)(o * 7U + 3U + F[f].id); }

pfs_err_t pfs_mount(pfs_ctx_t *c, const nand_hal_t *n, const pfs_nor_hal_t *r)
{
    (void)c; mount_calls++; g_nand = n; g_nor = r;
    assert(n->init() == NAND_OK && r->mount() == PFS_HAL_OK);
    /* what PFS may try at a mount: an erase, a bad-block mark, a NOR commit; all must fail */
    assert(n->erase_block(3) != NAND_OK);
    assert(n->mark_bad_block(4) != NAND_OK);
    assert(n->write_page(1, 1, NULL, NULL, NULL) != NAND_OK);
    assert(r->write_atomic("x", (const uint8_t *)"a", 1) != PFS_HAL_OK);
    return PFS_OK;
}
void pfs_get_status(pfs_ctx_t *c, pfs_status_t *o) { (void)c; memset(o, 0, sizeof *o); o->index_ready = true; }
uint16_t pfs_file_count(pfs_ctx_t *c) { (void)c; return 2; }
pfs_err_t pfs_file_info(pfs_ctx_t *c, uint16_t i, pfs_file_info_t *o)
{
    (void)c; const ff_t *f = &F[i]; memset(o, 0, sizeof *o);
    o->file_id = f->id; o->first_offset = (uint64_t)f->first_seq * PAY; o->size_bytes = ((uint64_t)f->last_seq + 1) * PAY;
    return PFS_OK;
}
pfs_err_t pfs_file_stat(pfs_ctx_t *c, uint16_t id, pfs_file_info_t *o)
{
    (void)c; const ff_t *f = id == 5 ? &F[0] : &F[1];
    pfs_file_info(c, id == 5 ? 0 : 1, o);
    if (f->exact) { o->size_bytes = (uint64_t)f->last_seq * PAY + f->last_len; o->flags |= PFS_FILE_SIZE_EXACT; }
    return f->exact ? PFS_OK : PFS_ERR_CORRUPT;
}
static int reads;
pfs_err_t pfs_read(pfs_ctx_t *c, uint16_t id, uint64_t off, uint8_t *dst, size_t len, size_t *out)
{
    (void)c; int fi = id == 5 ? 0 : 1; const ff_t *f = &F[fi]; reads++;
    *out = 0;
    assert(off % PAY == 0 && len == PAY);                    /* the backend reads whole pages */
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
static nand_status_t t_wr(uint16_t b, uint16_t p, const uint8_t *m, const uint8_t *o, nand_ecc_t *e) { (void)b; (void)p; (void)m; (void)o; (void)e; assert(!"real write"); return NAND_OK; }
static nand_status_t t_er(uint16_t b) { (void)b; assert(!"real erase"); return NAND_OK; }
static nand_status_t t_bad(uint16_t b, bool *x) { (void)b; *x = false; return NAND_OK; }
static nand_status_t t_mk(uint16_t b) { (void)b; assert(!"real mark"); return NAND_OK; }
static const nand_hal_t real = { t_init, t_rd, t_wr, t_er, t_bad, t_mk };
static pfs_hal_status_t n_m(void) { return PFS_HAL_OK; }
static pfs_hal_status_t n_r(const char *a, uint8_t *d, size_t m, size_t *l) { (void)a; (void)d; (void)m; *l = 0; return PFS_HAL_OK; }
static pfs_hal_status_t n_w(const char *a, const uint8_t *s, size_t l) { (void)a; (void)s; (void)l; assert(!"real NOR write"); return PFS_HAL_OK; }
static const pfs_nor_hal_t rnor = { n_m, n_r, n_w };

int main(void)
{
    assert(mtp_store_pfs_get() == NULL);                      /* not initialised: no store */
    assert(mtp_pfs_mount_readonly((pfs_ctx_t *)1, &real, &rnor) == PFS_OK);
    assert(mtp_pfs_blocked_writes() == 3 && mtp_pfs_blocked_nor_writes() == 1);
    assert(mtp_store_pfs_init((pfs_ctx_t *)1));
    const mtp_store_t *s = mtp_store_pfs_get();
    assert(s && !strcmp(s->name, "NAND") && s->present() && s->refresh() == 2 && s->count() == 2);
    mtp_file_info_t a, b;
    assert(s->info(0, &a) && s->info(1, &b) && !s->info(2, &a));
    s->info(0, &a);
    printf("file0 %s size=%llu exact=%d\nfile1 %s size=%llu exact=%d\n", a.name, (unsigned long long)a.size, a.size_exact, b.name, (unsigned long long)b.size, b.size_exact);
    /* file 0: trimmed, first_offset 1876 -> start 1880 (first whole record); size 4*1876+100-1880 */
    assert(!strcmp(a.name, "NAND_F00005D3.BIN") && a.size == 4U * PAY + 100U - 1880U && a.size_exact);
    assert(!strcmp(b.name, "NAND_F00007D3.BIN") && b.size == 6U * PAY && !b.size_exact);
    uint8_t buf[512];
    uint64_t off = 0; int32_t n;
    reads = 0;
    while ((n = s->read(0, off, buf, sizeof buf)) > 0) {          /* sequential read in 512-byte requests */
        for (int32_t i = 0; i < n; i++) { assert(buf[i] == byte_at(0, 1880U + off + (uint64_t)i)); }
        off += (uint64_t)n;
    }
    assert(n == 0 && off == a.size);
    printf("file0 read: %llu bytes, %d NAND page reads (cache works if 4)\n", (unsigned long long)off, reads);
    assert(reads == 4);
    /* random reads, crossing pages */
    assert(s->read(0, 3000, buf, 512) == 512 && buf[0] == byte_at(0, 4880) && buf[511] == byte_at(0, 4880 + 511));
    assert(s->read(0, a.size - 10, buf, 512) == 10);              /* short read at the end */
    assert(s->read(0, a.size, buf, 512) == 0 && s->read(9, 0, buf, 1) < 0);
    /* file 1: page 2 cannot be read: filler, the offsets stay true */
    off = 0; reads = 0;
    uint32_t ff = 0;
    while ((n = s->read(1, off, buf, sizeof buf)) > 0) {
        for (int32_t i = 0; i < n; i++) {
            uint64_t o = off + (uint64_t)i;
            if (o / PAY == 2) { assert(buf[i] == 0xFF); ff++; } else { assert(buf[i] == byte_at(1, o)); }
        }
        off += (uint64_t)n;
    }
    assert(off == b.size && ff == PAY && mtp_store_pfs_lost_pages() == 1);
    assert(s->capacity() == 1024ULL * 64U * PAY);
    printf("PASS (blocked NAND %u, NOR %u)\n", (unsigned)mtp_pfs_blocked_writes(), (unsigned)mtp_pfs_blocked_nor_writes());
    return 0;
}
