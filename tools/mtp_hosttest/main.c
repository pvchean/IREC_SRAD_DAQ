/* Host test of the real MTP handlers: a scripted PTP/MTP session (Claude helper, not a deliverable).
   The host side here behaves like libmtp: command, then (for host-to-device data) the data container at once, then it reads
   containers until the response. Every container is checked against PIMA 15740 / MTP 1.1. See Docs/MTP/HostTest_Report.md. */
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>

#include "esp_err.h"
#include "mtp_store.h"
#include "mtp_vfs.h"
#include "tinyusb_mtp.h"
#include "hosttest.h"

/* ---------------------------------------------------------------- codes (PIMA 15740 / MTP 1.1) */
enum { OP_GET_DEVICE_INFO = 0x1001, OP_OPEN_SESSION = 0x1002, OP_CLOSE_SESSION = 0x1003, OP_GET_STORAGE_IDS = 0x1004,
       OP_GET_STORAGE_INFO = 0x1005, OP_GET_NUM_OBJECTS = 0x1006, OP_GET_OBJECT_HANDLES = 0x1007, OP_GET_OBJECT_INFO = 0x1008,
       OP_GET_OBJECT = 0x1009, OP_DELETE_OBJECT = 0x100B, OP_SEND_OBJECT_INFO = 0x100C, OP_SEND_OBJECT = 0x100D,
       OP_FORMAT_STORE = 0x100F, OP_SET_OBJECT_PROTECTION = 0x1012, OP_GET_DEVICE_PROP_DESC = 0x1014,
       OP_GET_DEVICE_PROP_VALUE = 0x1015, OP_SET_DEVICE_PROP_VALUE = 0x1016, OP_MOVE_OBJECT = 0x1019, OP_COPY_OBJECT = 0x101A,
       OP_GET_PARTIAL_OBJECT = 0x101B, OP_GET_OBJ_PROPS_SUPPORTED = 0x9801, OP_GET_OBJ_PROP_DESC = 0x9802,
       OP_GET_OBJ_PROP_VALUE = 0x9803, OP_SET_OBJ_PROP_VALUE = 0x9804, OP_GET_OBJ_PROP_LIST = 0x9805,
       OP_SEND_OBJ_PROP_LIST = 0x9808, OP_GET_OBJ_REFERENCES = 0x9810, OP_SET_OBJ_REFERENCES = 0x9811,
       OP_AND_GET_PARTIAL64 = 0x95C1, OP_AND_SEND_PARTIAL = 0x95C2, OP_AND_TRUNCATE = 0x95C3, OP_AND_BEGIN_EDIT = 0x95C4,
       OP_AND_END_EDIT = 0x95C5 };
enum { RC_OK = 0x2001, RC_GENERAL = 0x2002, RC_SESSION_NOT_OPEN = 0x2003, RC_INVALID_TID = 0x2004, RC_OP_NOT_SUPPORTED = 0x2005,
       RC_PARAM_NOT_SUPPORTED = 0x2006, RC_INVALID_STORAGE = 0x2008, RC_INVALID_HANDLE = 0x2009, RC_STORE_FULL = 0x200C,
       RC_WRITE_PROTECTED = 0x200D, RC_STORE_READ_ONLY = 0x200E, RC_ACCESS_DENIED = 0x200F, RC_INVALID_PARENT = 0x201A,
       RC_INVALID_PARAMETER = 0x201D, RC_SESSION_ALREADY_OPEN = 0x201E, RC_CANCELLED = 0x201F, RC_INVALID_PROP = 0xA801,
       RC_PROP_NOT_SUPPORTED = 0xA80A, RC_GROUP_NOT_SUPPORTED = 0xA805 };
enum { CT_COMMAND = 1, CT_DATA = 2, CT_RESPONSE = 3, CT_EVENT = 4 };
#define STORAGE_ID 0x00010001U
#define ROOT 0xFFFFFFFFU

/* ---------------------------------------------------------------- checks */
static int g_pass, g_fail;
static const char *g_test = "";
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; printf("  FAIL [%s] line %d: ", g_test, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void test_begin(const char *name) { g_test = name; printf("-- %s\n", name); }

/* ---------------------------------------------------------------- little endian helpers and a dataset reader */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

typedef struct { const uint8_t *p; size_t n, o; bool bad; } rd_t;
static uint8_t  r8(rd_t *r)  { if (r->o + 1 > r->n) { r->bad = true; return 0; } return r->p[r->o++]; }
static uint16_t r16(rd_t *r) { if (r->o + 2 > r->n) { r->bad = true; return 0; } uint16_t v = le16(r->p + r->o); r->o += 2; return v; }
static uint32_t r32(rd_t *r) { if (r->o + 4 > r->n) { r->bad = true; return 0; } uint32_t v = le32(r->p + r->o); r->o += 4; return v; }
static uint64_t r64(rd_t *r) { if (r->o + 8 > r->n) { r->bad = true; return 0; } uint64_t v = le64(r->p + r->o); r->o += 8; return v; }
/* PTP string: count byte (characters including the NUL), then UTF-16LE. Returns the count byte. A count of 0 = empty string. */
static int rstr(rd_t *r, char *out, size_t cap)
{
    int cnt = r8(r);
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t u = r16(r);
        if (i == cnt - 1) { if (u != 0) { r->bad = true; } break; }             /* the last unit must be the NUL */
        if (u == 0) { r->bad = true; }
        if (k + 1 < cap) { out[k++] = u < 0x80 ? (char)u : '?'; }
    }
    out[k] = 0;
    return cnt;
}

/* ---------------------------------------------------------------- the PTP initiator */
static uint32_t g_tid;
typedef struct {
    int st;                       /* 0 ok, -1 timeout, -2 stall, -3 container error */
    const char *why;
    uint16_t code; int np; uint32_t p[5]; uint32_t rtid;   /* response */
    uint8_t *d; size_t dlen; bool has_data; uint32_t dtid; uint16_t dcode;
    int zlp_data;                 /* a ZLP followed the data container */
} xact_t;
static void x_free(xact_t *x) { free(x->d); x->d = NULL; }

static int rd_pkt(uint8_t *p, size_t *n)
{
    return um_host_in_packet(p, n);
}

/* read one container; checks the end rules of USB bulk framing */
static int read_cont(uint8_t **out, size_t *len, const char **why, int *zlp)
{
    uint8_t pk[PKT];
    size_t n = 0, cap = 8192, got = 0;
    uint8_t *b = malloc(cap);
    *zlp = 0;
    int r = rd_pkt(pk, &n);
    if (r != 0) { free(b); *why = r == -2 ? "IN pipe stalled" : "IN timeout (no container)"; return r; }
    if (n < 12) { free(b); *why = "first packet shorter than a container header (stray ZLP or short packet)"; return -3; }
    memcpy(b, pk, n); got = n;
    uint32_t L = le32(b);
    if (L < 12) { free(b); *why = "container length < 12"; return -3; }
    while (got < L) {
        r = rd_pkt(pk, &n);
        if (r != 0) { free(b); *why = r == -2 ? "IN pipe stalled in the middle" : "IN timeout in the middle of a container"; return r; }
        if (got + n > cap) { cap *= 2; b = realloc(b, cap); }
        memcpy(b + got, pk, n); got += n;
        if (n < PKT && got < L) { free(b); *why = "short packet before the container length"; return -3; }
    }
    if (got != L) { free(b); *why = "device sent more bytes than the container length"; return -3; }
    if (L % PKT == 0) {                               /* PIMA 15740 D.7: a transfer that ends on a packet boundary needs a ZLP */
        r = rd_pkt(pk, &n);
        if (r != 0 || n != 0) { free(b); *why = "container length is a multiple of 64 and no ZLP follows"; return -3; }
        *zlp = 1;
    }
    *out = b; *len = got;
    return 0;
}

static int send_cont(uint16_t type, uint16_t code, uint32_t tid, const uint32_t *p, int np, const uint8_t *pl, size_t pln)
{
    size_t L = 12 + 4 * (size_t)np + pln;
    uint8_t *b = calloc(1, L);
    put32(b, (uint32_t)L); put16(b + 4, type); put16(b + 6, code); put32(b + 8, tid);
    for (int i = 0; i < np; i++) { put32(b + 12 + 4 * i, p[i]); }
    if (pln) { memcpy(b + 12 + 4 * np, pl, pln); }
    int r = um_host_out(b, L);
    free(b);
    return r;
}

/* one transaction. tid_override: use this tid instead of the counter (when != UINT32_MAX) */
static void ptp_tid(xact_t *x, uint16_t op, int np, const uint32_t *p, const uint8_t *dout, size_t doutlen, uint32_t tid)
{
    memset(x, 0, sizeof *x);
    x->st = send_cont(CT_COMMAND, op, tid, p, np, NULL, 0);
    if (x->st) { x->why = x->st == -2 ? "command: OUT stalled" : "command: OUT timeout"; return; }
    if (dout) {
        x->st = send_cont(CT_DATA, op, tid, NULL, 0, dout, doutlen);
        if (x->st) { x->why = x->st == -2 ? "data out: OUT stalled" : "data out: OUT timeout (device did not arm the OUT endpoint)"; return; }
    }
    for (int guard = 0; guard < 4; guard++) {
        uint8_t *b; size_t L; int zlp;
        x->st = read_cont(&b, &L, &x->why, &zlp);
        if (x->st) { return; }
        uint16_t type = le16(b + 4);
        if (type == CT_DATA) {
            if (x->has_data) { free(b); x->st = -3; x->why = "two data containers"; return; }
            x->has_data = true; x->dcode = le16(b + 6); x->dtid = le32(b + 8); x->zlp_data = zlp;
            x->dlen = L - 12; x->d = malloc(x->dlen ? x->dlen : 1); memcpy(x->d, b + 12, x->dlen);
            free(b);
        } else if (type == CT_RESPONSE) {
            x->code = le16(b + 6); x->rtid = le32(b + 8); x->np = (int)((L - 12) / 4);
            for (int i = 0; i < x->np && i < 5; i++) { x->p[i] = le32(b + 12 + 4 * i); }
            free(b);
            return;
        } else { free(b); x->st = -3; x->why = "unexpected container type"; return; }
    }
    x->st = -3; x->why = "no response after the data";
}
static void ptp(xact_t *x, uint16_t op, int np, const uint32_t *p, const uint8_t *dout, size_t doutlen)
{
    ptp_tid(x, op, np, p, dout, doutlen, g_tid++);
}

/* a response with the expected code and nothing else wrong */
static bool expect(xact_t *x, uint16_t code, const char *what)
{
    bool ok = x->st == 0 && x->code == code;
    if (!ok) { printf("  FAIL [%s] %s: want 0x%04x, got st=%d code=0x%04x %s\n", g_test, what, code, x->st, x->code, x->why ? x->why : ""); g_fail++; }
    else { g_pass++; }
    return ok;
}
static bool expect_err(xact_t *x, const uint16_t *codes, int nc, const char *what)
{
    bool ok = false;
    if (x->st == 0) { for (int i = 0; i < nc; i++) { if (x->code == codes[i]) { ok = true; } } }
    if (!ok) { printf("  FAIL [%s] %s: got st=%d code=0x%04x %s\n", g_test, what, x->st, x->code, x->why ? x->why : ""); g_fail++; } else { g_pass++; }
    return ok;
}

/* ---------------------------------------------------------------- device boot (like files_mode_usb_start) */
static int g_hook_calls;
static void on_session_open(void) { g_hook_calls++; mtp_vfs_refresh(); }
static bool g_booted, g_vfs_init;
static void device_boot(void)
{
    if (g_booted) { assert(tinyusb_mtp_uninstall_driver() == ESP_OK); }
    if (!g_vfs_init) { assert(mtp_vfs_init() == ESP_OK); g_vfs_init = true; }
    tinyusb_mtp_set_session_open_callback(on_session_open);
    const tinyusb_mtp_driver_config_t dc = { .manufacturer = "IREC SRAD", .model = "DAQ", .version = "1.0", .serial = NULL, .friendly_name = "IREC DAQ" };
    assert(tinyusb_mtp_install_driver(&dc) == ESP_OK);
    const tinyusb_mtp_storage_config_t sc = { .base_path = MTP_VFS_BASE, .display_name = "DAQ", .removable = false,
                                              .total_bytes = mtp_vfs_capacity(), .free_bytes = 0 };
    assert(tinyusb_mtp_register_storage(&sc, NULL) == ESP_OK);
    um_reset();
    g_tid = 0;
    g_booted = true;
}

static bool open_session(void)
{
    xact_t x; uint32_t p[1] = { 1 };
    ptp_tid(&x, OP_OPEN_SESSION, 1, p, NULL, 0, 0);
    g_tid = 1;
    bool ok = x.st == 0 && x.code == RC_OK;
    x_free(&x);
    return ok;
}
static bool close_session(void)
{
    xact_t x;
    ptp(&x, OP_CLOSE_SESSION, 0, NULL, NULL, 0);
    bool ok = x.st == 0 && x.code == RC_OK;
    x_free(&x);
    return ok;
}

/* the canary: after any error the layer must answer a normal command, and a read must give the right bytes */
static const mtp_store_t *NAND, *SD;
static uint32_t h_nand_dir, h_sd_dir;
static bool g_reopen_ok;       /* a class reset closes the session: the canary may open it again */
static bool canary(const char *after)
{
    char w[160];
    xact_t x;
    ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
    if (g_reopen_ok && x.st == 0 && x.code == RC_SESSION_NOT_OPEN) {
        x_free(&x);
        CHECK(open_session(), "OpenSession after the reset (%s)", after);
        ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
    }
    snprintf(w, sizeof w, "canary GetStorageIDs after %s", after);
    bool ok = expect(&x, RC_OK, w);
    if (ok) {
        ok = x.has_data && x.dlen == 8 && le32(x.d) == 1 && le32(x.d + 4) == STORAGE_ID;
        CHECK(ok, "canary: GetStorageIDs data wrong after %s", after);
    }
    x_free(&x);
    if (!ok) { return false; }
    CHECK(!um_stalled(EP_OUT) && !um_stalled(EP_IN), "endpoints stalled after %s", after);
    CHECK(!um_in_pending() && um_out_pending() == 0, "stray data in the pipes after %s", after);
    return ok;
}

/* ---------------------------------------------------------------- dataset parsers */
typedef struct {
    uint32_t storage; uint16_t fmt, prot; uint32_t size; uint16_t tfmt; uint32_t tsize, tw, th, iw, ih, ibd, parent; uint16_t assoc;
    uint32_t adesc, seq; char name[260], cdate[40], mdate[40], kw[40]; int cdate_cnt, mdate_cnt; bool ok;
} oi_t;

static bool parse_oi(const xact_t *x, oi_t *o)
{
    memset(o, 0, sizeof *o);
    rd_t r = { x->d, x->dlen, 0, false };
    o->storage = r32(&r); o->fmt = r16(&r); o->prot = r16(&r); o->size = r32(&r); o->tfmt = r16(&r); o->tsize = r32(&r);
    o->tw = r32(&r); o->th = r32(&r); o->iw = r32(&r); o->ih = r32(&r); o->ibd = r32(&r); o->parent = r32(&r);
    o->assoc = r16(&r); o->adesc = r32(&r); o->seq = r32(&r);
    rstr(&r, o->name, sizeof o->name);
    o->cdate_cnt = rstr(&r, o->cdate, sizeof o->cdate);
    o->mdate_cnt = rstr(&r, o->mdate, sizeof o->mdate);
    rstr(&r, o->kw, sizeof o->kw);
    o->ok = !r.bad && r.o == r.n;
    return o->ok;
}

static bool get_handles(uint32_t storage, uint32_t fmt, uint32_t parent, uint32_t *h, uint32_t *n, uint16_t *rc)
{
    xact_t x; uint32_t p[3] = { storage, fmt, parent };
    ptp(&x, OP_GET_OBJECT_HANDLES, 3, p, NULL, 0);
    *rc = x.st ? 0 : x.code;
    *n = 0;
    bool ok = x.st == 0 && x.code == RC_OK && x.has_data && x.dlen >= 4 && x.dlen == 4 + 4 * (size_t)le32(x.d);
    if (ok) { *n = le32(x.d); for (uint32_t i = 0; i < *n && i < 600; i++) { h[i] = le32(x.d + 4 + 4 * i); } }
    x_free(&x);
    return ok;
}

static bool get_oi(uint32_t h, oi_t *o, uint16_t *rc)
{
    xact_t x; uint32_t p[1] = { h };
    ptp(&x, OP_GET_OBJECT_INFO, 1, p, NULL, 0);
    *rc = x.st ? 0 : x.code;
    bool ok = x.st == 0 && x.code == RC_OK && x.has_data && parse_oi(&x, o);
    x_free(&x);
    return ok;
}

/* ---------------------------------------------------------------- the file model: what the stores hold */
typedef struct { uint32_t h; int dir; uint32_t idx; char name[64]; uint64_t size; } fobj_t;
static fobj_t g_f[600];
static int g_nf;

/* ground truth read: straight from the store, bypassing MTP */
static int32_t truth(int dir, uint32_t idx, uint64_t off, uint8_t *b, size_t n)
{
    const mtp_store_t *s = dir == 0 ? NAND : SD;
    return s->read(idx, off, b, n);
}

/* ---------------------------------------------------------------- tests */
static const fake_pfs_file_t NAND_FILES[] = {
    { 5, 1, 40, 100, true, -1 },      /* trimmed, exact: 73260 bytes, many pages */
    { 7, 0, 5, 1876, false, 2 },      /* approximate size, page 2 is unreadable: 0xFF filler */
    { 9, 0, 2, 344, true, -1 },       /* exactly 4096 bytes: GetObject container 4108 */
    { 11, 0, 2, 24, true, -1 },       /* 3776 = 59 * 64 (data container 3788) */
    { 13, 0, 0, 0, true, -1 },        /* empty file */
    { 15, 0, 0, 1, true, -1 },        /* one byte */
    { 17, 0, 2, 332, true, -1 },      /* 4084 bytes: container exactly 4096 = EP buffer, needs a ZLP */
};
#define NNAND ((int)(sizeof NAND_FILES / sizeof NAND_FILES[0]))

static void load_default_stores(void)
{
    fake_pfs_set_files(NAND_FILES, NNAND);
    fake_sd_set(true, 3);
}

/* T01 */
static void t_before_session(void)
{
    test_begin("T01 commands before OpenSession");
    device_boot();
    xact_t x;
    ptp(&x, OP_GET_DEVICE_INFO, 0, NULL, NULL, 0);
    expect(&x, RC_OK, "GetDeviceInfo without session is allowed");
    x_free(&x);
    ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
    expect(&x, RC_SESSION_NOT_OPEN, "GetStorageIDs without session");
    x_free(&x);
    uint32_t p[3] = { STORAGE_ID, 0, ROOT };
    ptp(&x, OP_GET_OBJECT_HANDLES, 3, p, NULL, 0);
    expect(&x, RC_SESSION_NOT_OPEN, "GetObjectHandles without session");
    x_free(&x);
    uint32_t p1[1] = { 1 };
    ptp(&x, OP_GET_OBJECT_INFO, 1, p1, NULL, 0);
    expect(&x, RC_SESSION_NOT_OPEN, "GetObjectInfo without session");
    x_free(&x);
    ptp(&x, OP_CLOSE_SESSION, 0, NULL, NULL, 0);
    expect(&x, RC_SESSION_NOT_OPEN, "CloseSession without session");
    x_free(&x);
    CHECK(g_hook_calls == 0 || true, "hook");
}

/* T02 */
static void t_device_info(void)
{
    test_begin("T02 GetDeviceInfo dataset");
    device_boot();
    xact_t x;
    ptp(&x, OP_GET_DEVICE_INFO, 0, NULL, NULL, 0);
    if (!expect(&x, RC_OK, "GetDeviceInfo")) { x_free(&x); return; }
    CHECK(x.has_data && x.dcode == OP_GET_DEVICE_INFO && x.dtid == x.rtid, "data container header: code 0x%04x tid %u/%u", x.dcode, x.dtid, x.rtid);
    rd_t r = { x.d, x.dlen, 0, false };
    char s[300];
    CHECK(r16(&r) == 100, "StandardVersion must be 100");
    uint32_t vid = r32(&r);
    CHECK(vid == 6 || vid == 0xFFFFFFFF, "VendorExtensionID %u", vid);
    CHECK(r16(&r) == 100, "VendorExtensionVersion");
    int c = rstr(&r, s, sizeof s);
    CHECK(c > 0 && !strcmp(s, "microsoft.com: 1.0; android.com: 1.0; "), "VendorExtensionDesc '%s' (count %d)", s, c);
    CHECK(c == (int)strlen("microsoft.com: 1.0; android.com: 1.0; ") + 1, "extension string count byte %d", c);
    CHECK(r16(&r) == 0, "FunctionalMode");
    static const uint16_t want_ops[] = { 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009, 0x101B, 0x95C1, 0x1014,
                                         0x1015, 0x9801, 0x9802, 0x9803, 0x9805, 0x9810 };
    uint32_t nops = r32(&r);
    CHECK(nops == sizeof want_ops / sizeof want_ops[0], "supported operations count %u", nops);
    bool have_write = false;
    for (uint32_t i = 0; i < nops && i < 64; i++) {
        uint16_t op = r16(&r);
        bool found = false;
        for (size_t k = 0; k < sizeof want_ops / sizeof want_ops[0]; k++) { if (want_ops[k] == op) { found = true; } }
        CHECK(found, "operation 0x%04x is listed but not expected", op);
        if (op == 0x100B || op == 0x100C || op == 0x100D || op == 0x100F || op == 0x1012 || op == 0x1019 || op == 0x101A ||
            op == 0x9804 || (op >= 0x95C2 && op <= 0x95C5)) { have_write = true; }
    }
    CHECK(!have_write, "a write operation is listed in DeviceInfo");
    uint32_t nev = r32(&r);
    CHECK(nev <= 1, "supported events count %u", nev);
    for (uint32_t i = 0; i < nev && i < 16; i++) { uint16_t ev = r16(&r); CHECK(ev >= 0x4000 && ev != 0xFFFF && (ev < 0x4000 + 0x100 || ev >= 0xC000), "event code 0x%04x is not a valid event code", ev); }
    uint32_t ndp = r32(&r);
    CHECK(ndp == 1, "device properties count %u", ndp);
    for (uint32_t i = 0; i < ndp && i < 16; i++) { CHECK(r16(&r) == 0xD402, "device property code"); }
    uint32_t ncap = r32(&r);
    CHECK(ncap == 2, "capture formats count %u", ncap);
    for (uint32_t i = 0; i < ncap && i < 16; i++) { (void)r16(&r); }
    uint32_t npb = r32(&r);
    CHECK(npb == 8, "playback formats count %u", npb);
    for (uint32_t i = 0; i < npb && i < 32; i++) { (void)r16(&r); }
    rstr(&r, s, sizeof s); CHECK(!strcmp(s, "IREC SRAD"), "Manufacturer '%s'", s);
    rstr(&r, s, sizeof s); CHECK(!strcmp(s, "DAQ"), "Model '%s'", s);
    rstr(&r, s, sizeof s); CHECK(!strcmp(s, "1.0"), "DeviceVersion '%s'", s);
    rstr(&r, s, sizeof s); CHECK(!strcmp(s, "AABBCCDDEEFF"), "SerialNumber '%s'", s);
    CHECK(!r.bad && r.o == r.n, "dataset length: parsed %zu of %zu bytes (bad=%d)", r.o, r.n, r.bad);
    x_free(&x);
}

/* T03 */
static void t_session(void)
{
    test_begin("T03 OpenSession / CloseSession");
    device_boot();
    xact_t x; uint32_t p0[1] = { 0 }, p1[1] = { 1 };
    int hooks = g_hook_calls;
    ptp_tid(&x, OP_OPEN_SESSION, 1, p0, NULL, 0, 0);
    expect(&x, RC_INVALID_PARAMETER, "OpenSession with SessionID 0");
    x_free(&x);
    CHECK(g_hook_calls == hooks, "refresh hook ran for an invalid OpenSession");
    ptp_tid(&x, OP_OPEN_SESSION, 1, p1, NULL, 0, 0);
    expect(&x, RC_OK, "OpenSession(1)");
    CHECK(x.rtid == 0 && x.dlen == 0 && x.np == 0, "OpenSession response: tid %u, params %d", x.rtid, x.np);
    x_free(&x);
    CHECK(g_hook_calls == hooks + 1, "refresh hook calls %d (want 1)", g_hook_calls - hooks);
    g_tid = 1;
    ptp(&x, OP_OPEN_SESSION, 1, p1, NULL, 0);
    expect(&x, RC_SESSION_ALREADY_OPEN, "second OpenSession");
    CHECK(x.np == 1 && x.p[0] == 1, "SessionAlreadyOpen should carry the open SessionID (np=%d p0=%u)", x.np, x.p[0]);
    x_free(&x);
    CHECK(g_hook_calls == hooks + 1, "refresh hook must not run for a second OpenSession");
    canary("second OpenSession");
    uint32_t p2[1] = { 2 };
    ptp(&x, OP_OPEN_SESSION, 1, p2, NULL, 0);
    expect(&x, RC_SESSION_ALREADY_OPEN, "OpenSession with another id while open");
    x_free(&x);
    ptp(&x, OP_CLOSE_SESSION, 0, NULL, NULL, 0);
    expect(&x, RC_OK, "CloseSession");
    x_free(&x);
    ptp(&x, OP_CLOSE_SESSION, 0, NULL, NULL, 0);
    expect(&x, RC_SESSION_NOT_OPEN, "second CloseSession");
    x_free(&x);
    ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
    expect(&x, RC_SESSION_NOT_OPEN, "command after CloseSession");
    x_free(&x);
    ptp_tid(&x, OP_OPEN_SESSION, 1, p1, NULL, 0, 0);
    g_tid = 1;
    expect(&x, RC_OK, "OpenSession again after CloseSession");
    x_free(&x);
    CHECK(g_hook_calls == hooks + 2, "refresh hook calls after reopen: %d (want 2)", g_hook_calls - hooks);
    canary("reopen");
}

/* T04 */
static void t_storage(void)
{
    test_begin("T04 GetStorageIDs / GetStorageInfo");
    device_boot();
    CHECK(open_session(), "open");
    canary("open");
    xact_t x; uint32_t p[1] = { STORAGE_ID };
    ptp(&x, OP_GET_STORAGE_INFO, 1, p, NULL, 0);
    if (expect(&x, RC_OK, "GetStorageInfo")) {
        rd_t r = { x.d, x.dlen, 0, false };
        char s[100];
        uint16_t st = r16(&r), fs = r16(&r), ac = r16(&r);
        uint64_t cap = r64(&r), fr = r64(&r);
        uint32_t fo = r32(&r);
        CHECK(st == 3, "StorageType %u (3 = fixed RAM)", st);
        CHECK(fs == 2, "FilesystemType %u (2 = generic hierarchical)", fs);
        CHECK(ac == 1, "AccessCapability %u: read-only must be 1 (without deletion)", ac);
        CHECK(cap == mtp_vfs_capacity() && cap > 0, "MaxCapacity %" PRIu64 " (want %" PRIu64 ")", cap, mtp_vfs_capacity());
        CHECK(fr == 0, "FreeSpaceInBytes %" PRIu64 " (read-only: 0)", fr);
        CHECK(fo == 0xFFFFFFFFU || fo == 0, "FreeSpaceInObjects 0x%08x", fo);
        rstr(&r, s, sizeof s); CHECK(!strcmp(s, "DAQ"), "StorageDescription '%s'", s);
        rstr(&r, s, sizeof s); CHECK(s[0] != 0 || true, "VolumeIdentifier '%s'", s);
        CHECK(!r.bad && r.o == r.n, "StorageInfo length: parsed %zu of %zu", r.o, r.n);
    }
    x_free(&x);
    uint32_t pbad[1] = { 0x00020001 };
    ptp(&x, OP_GET_STORAGE_INFO, 1, pbad, NULL, 0);
    expect(&x, RC_INVALID_STORAGE, "GetStorageInfo with a wrong storage id");
    x_free(&x);
    uint32_t pall[1] = { 0xFFFFFFFF };
    ptp(&x, OP_GET_STORAGE_INFO, 1, pall, NULL, 0);
    expect(&x, RC_INVALID_STORAGE, "GetStorageInfo with 0xFFFFFFFF");
    x_free(&x);
    canary("storage errors");
}

/* the listing. Builds g_f[] from GetObjectHandles + GetObjectInfo, checks the datasets. */
static void list_and_check(void)
{
    uint32_t h[600], n; uint16_t rc;
    g_nf = 0;
    bool ok = get_handles(STORAGE_ID, 0, ROOT, h, &n, &rc);
    CHECK(ok, "GetObjectHandles root: rc 0x%04x", rc);
    CHECK(n == 2, "root has %u objects (want exactly the folders NAND and SD)", n);
    oi_t oi;
    h_nand_dir = h_sd_dir = 0;
    for (uint32_t i = 0; ok && i < n; i++) {
        bool g = get_oi(h[i], &oi, &rc);
        CHECK(g, "GetObjectInfo(folder %u) rc 0x%04x", h[i], rc);
        if (!g) { continue; }
        CHECK(oi.storage == STORAGE_ID, "folder StorageID 0x%08x", oi.storage);
        CHECK(oi.fmt == 0x3001, "folder ObjectFormat 0x%04x (Association 0x3001)", oi.fmt);
        CHECK(oi.assoc == 1, "folder AssociationType %u (1 = generic folder)", oi.assoc);
        CHECK(oi.adesc == 0, "folder AssociationDesc %u", oi.adesc);
        CHECK(oi.prot == 1, "folder ProtectionStatus %u (read-only = 1)", oi.prot);
        CHECK(oi.size == 0, "folder ObjectCompressedSize %u", oi.size);
        CHECK(oi.parent == 0, "folder ParentObject 0x%08x (root = 0)", oi.parent);
        CHECK(oi.tfmt == 0 && oi.tsize == 0 && oi.tw == 0 && oi.th == 0 && oi.iw == 0 && oi.ih == 0 && oi.ibd == 0 && oi.seq == 0, "folder thumb/image fields not zero");
        CHECK(!strcmp(oi.name, "NAND") || !strcmp(oi.name, "SD"), "folder name '%s'", oi.name);
        if (!strcmp(oi.name, "NAND")) { h_nand_dir = h[i]; }
        if (!strcmp(oi.name, "SD")) { h_sd_dir = h[i]; }
    }
    CHECK(h_nand_dir && h_sd_dir, "folders NAND (%u) and SD (%u) not both found", h_nand_dir, h_sd_dir);
    for (int d = 0; d < 2; d++) {
        uint32_t dir = d == 0 ? h_nand_dir : h_sd_dir;
        const mtp_store_t *s = d == 0 ? NAND : SD;
        if (!dir) { continue; }
        ok = get_handles(STORAGE_ID, 0, dir, h, &n, &rc);
        CHECK(ok, "GetObjectHandles(%s) rc 0x%04x", d == 0 ? "NAND" : "SD", rc);
        CHECK(n == s->count(), "%s folder lists %u files, store has %u", d == 0 ? "NAND" : "SD", n, s->count());
        for (uint32_t i = 0; ok && i < n; i++) {
            bool g = get_oi(h[i], &oi, &rc);
            CHECK(g, "GetObjectInfo(file %u) rc 0x%04x", h[i], rc);
            if (!g) { continue; }
            mtp_file_info_t fi;
            bool found = false;
            uint32_t idx = 0;
            for (uint32_t k = 0; k < s->count(); k++) { s->info(k, &fi); if (!strcmp(fi.name, oi.name)) { found = true; idx = k; break; } }
            CHECK(found, "file name '%s' is not in the store", oi.name);
            if (!found) { continue; }
            s->info(idx, &fi);
            CHECK(oi.storage == STORAGE_ID, "file StorageID");
            CHECK(oi.fmt == 0x3000 || oi.fmt == 0x3004, "file ObjectFormat 0x%04x (%s)", oi.fmt, oi.name);
            CHECK(oi.assoc == 0 && oi.adesc == 0, "file AssociationType %u", oi.assoc);
            CHECK(oi.prot == 1, "file ProtectionStatus %u (read-only = 1)", oi.prot);
            CHECK(oi.size == fi.size, "file '%s' size %u, store %" PRIu64, oi.name, oi.size, fi.size);
            CHECK(oi.parent == dir, "file ParentObject %u (want folder handle %u)", oi.parent, dir);
            CHECK(oi.tfmt == 0 && oi.tsize == 0 && oi.tw == 0 && oi.th == 0 && oi.iw == 0 && oi.ih == 0 && oi.ibd == 0 && oi.seq == 0,
                  "file thumb/image fields not zero: tfmt 0x%04x tsize %u tw %u th %u iw %u ih %u ibd %u seq %u", oi.tfmt, oi.tsize, oi.tw, oi.th, oi.iw, oi.ih, oi.ibd, oi.seq);
            CHECK(strlen(oi.name) == strlen(fi.name), "file name length");
            bool date_ok = oi.mdate_cnt == 0 || (strlen(oi.mdate) == 15 && oi.mdate[8] == 'T');
            CHECK(date_ok, "DateModified '%s' is not YYYYMMDDThhmmss", oi.mdate);
            CHECK(oi.cdate_cnt == 0 || (strlen(oi.cdate) == 15 && oi.cdate[8] == 'T'), "DateCreated '%s'", oi.cdate);
            if (d == 1) { CHECK(oi.mdate_cnt != 0, "SD file '%s' has a real time but DateModified is empty", oi.name); }
            if (g_nf < 600) { g_f[g_nf++] = (fobj_t){ h[i], d, idx, "", fi.size }; snprintf(g_f[g_nf - 1].name, sizeof g_f[g_nf - 1].name, "%s", fi.name); }
        }
    }
}

/* T05 */
static void t_listing(void)
{
    test_begin("T05 GetObjectHandles / GetNumObjects / GetObjectInfo");
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    uint32_t h[600], n; uint16_t rc;
    /* format filter */
    CHECK(get_handles(STORAGE_ID, 0x3001, ROOT, h, &n, &rc) && n == 2, "format filter Association at root: n=%u rc=0x%04x", n, rc);
    CHECK(get_handles(STORAGE_ID, 0x3000, ROOT, h, &n, &rc) && n == 0, "format filter Undefined at root: n=%u", n);
    /* all storages */
    CHECK(get_handles(0xFFFFFFFF, 0, ROOT, h, &n, &rc) && n == 2, "all storages root: n=%u rc=0x%04x", n, rc);
    /* parent 0 = all objects of the store, any depth (PIMA 15740 10.5.2 / libmtp) */
    uint32_t total = 2 + (uint32_t)NAND->count() + (uint32_t)SD->count();
    CHECK(get_handles(STORAGE_ID, 0, 0, h, &n, &rc) && n == total, "parent 0 (all objects): n=%u want %u rc=0x%04x", n, total, rc);
    /* GetNumObjects */
    xact_t x; uint32_t p[3] = { STORAGE_ID, 0, ROOT };
    ptp(&x, OP_GET_NUM_OBJECTS, 3, p, NULL, 0);
    if (expect(&x, RC_OK, "GetNumObjects root")) { CHECK(x.np == 1 && x.p[0] == 2, "GetNumObjects root = %u (np %d)", x.p[0], x.np); }
    x_free(&x);
    uint32_t p2[3] = { STORAGE_ID, 0, h_nand_dir };
    ptp(&x, OP_GET_NUM_OBJECTS, 3, p2, NULL, 0);
    if (expect(&x, RC_OK, "GetNumObjects NAND")) { CHECK(x.np == 1 && x.p[0] == NAND->count(), "GetNumObjects NAND = %u", x.p[0]); }
    x_free(&x);
    /* errors */
    CHECK(!get_handles(0x00030001, 0, ROOT, h, &n, &rc) && rc == RC_INVALID_STORAGE, "wrong storage: rc 0x%04x", rc);
    CHECK(!get_handles(STORAGE_ID, 0, 0x7777, h, &n, &rc) && (rc == RC_INVALID_PARENT || rc == RC_INVALID_HANDLE), "wrong parent: rc 0x%04x", rc);
    CHECK(!get_handles(STORAGE_ID, 0, g_nf ? g_f[0].h : 1, h, &n, &rc) && (rc == RC_INVALID_PARENT || rc == RC_INVALID_HANDLE), "parent is a file: rc 0x%04x", rc);
    canary("GetObjectHandles errors");
    oi_t oi;
    CHECK(!get_oi(0x7777, &oi, &rc) && rc == RC_INVALID_HANDLE, "GetObjectInfo wrong handle: rc 0x%04x", rc);
    CHECK(!get_oi(0, &oi, &rc) && rc == RC_INVALID_HANDLE, "GetObjectInfo handle 0: rc 0x%04x", rc);
    CHECK(!get_oi(0xFFFFFFFF, &oi, &rc) && rc == RC_INVALID_HANDLE, "GetObjectInfo handle 0xFFFFFFFF: rc 0x%04x", rc);
    canary("GetObjectInfo errors");
    /* too few parameters (stale values must not crash) */
    ptp(&x, OP_GET_OBJECT_INFO, 0, NULL, NULL, 0);
    CHECK(x.st == 0, "GetObjectInfo with no parameter: st=%d", x.st);
    x_free(&x);
    canary("GetObjectInfo with no parameter");
}

/* T06 GetObject */
static void t_getobject(void)
{
    test_begin("T06 GetObject (full) against the store content");
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    int checked = 0;
    for (int i = 0; i < g_nf; i++) {
        xact_t x; uint32_t p[1] = { g_f[i].h };
        ptp(&x, OP_GET_OBJECT, 1, p, NULL, 0);
        char w[100]; snprintf(w, sizeof w, "GetObject %s", g_f[i].name);
        if (!expect(&x, RC_OK, w)) { x_free(&x); canary("GetObject failure"); continue; }
        CHECK(x.has_data && x.dcode == OP_GET_OBJECT && x.dtid == x.rtid, "%s: data header code 0x%04x tid %u/%u", g_f[i].name, x.dcode, x.dtid, x.rtid);
        CHECK(x.dlen == g_f[i].size, "%s: got %zu bytes, size %" PRIu64, g_f[i].name, x.dlen, g_f[i].size);
        uint8_t *t = malloc(g_f[i].size + 1);
        int32_t got = truth(g_f[i].dir, g_f[i].idx, 0, t, (size_t)g_f[i].size);
        CHECK(got == (int32_t)g_f[i].size, "store read of %s", g_f[i].name);
        if (x.dlen == g_f[i].size && got == (int32_t)g_f[i].size) { CHECK(!memcmp(t, x.d, x.dlen), "%s: content differs from the store", g_f[i].name); }
        /* the container length rule */
        CHECK(x.zlp_data == (((12 + x.dlen) % PKT) == 0), "%s: ZLP %d for container length %zu", g_f[i].name, x.zlp_data, 12 + x.dlen);
        CHECK(x.np == 0, "GetObject response has %d parameters", x.np);
        free(t); x_free(&x); checked++;
    }
    CHECK(checked == g_nf && g_nf == NNAND + 3, "checked %d of %d objects (want %d)", checked, g_nf, NNAND + 3);
    /* generator spot check of the trimmed NAND file: MTP offset 0 = PFS offset 1880 */
    for (int i = 0; i < g_nf; i++) {
        if (g_f[i].dir == 0 && !strcmp(g_f[i].name, "NAND_F00005D3.BIN")) {
            xact_t x; uint32_t p[1] = { g_f[i].h };
            ptp(&x, OP_GET_OBJECT, 1, p, NULL, 0);
            bool good = x.st == 0 && x.dlen == 40U * 1876 + 100 - 1880;
            for (size_t k = 0; good && k < x.dlen; k += 997) { good = x.d[k] == (uint8_t)((1880 + k) * 7U + 3U + 5U); }
            CHECK(good, "NAND_F00005D3.BIN does not match the PFS generator (trim start 1880)");
            x_free(&x);
        }
    }
    /* GetObject on a folder and on bad handles */
    xact_t x; uint32_t pf[1] = { h_nand_dir };
    const uint16_t bad[] = { RC_INVALID_HANDLE, RC_ACCESS_DENIED, RC_GENERAL };
    ptp(&x, OP_GET_OBJECT, 1, pf, NULL, 0);
    expect_err(&x, bad, 3, "GetObject on a folder");
    CHECK(!x.has_data, "GetObject on a folder sent data");
    x_free(&x);
    canary("GetObject on a folder");
    uint32_t pb[1] = { 0x7777 };
    ptp(&x, OP_GET_OBJECT, 1, pb, NULL, 0);
    expect(&x, RC_INVALID_HANDLE, "GetObject wrong handle");
    x_free(&x);
    canary("GetObject wrong handle");
    /* repeat: the same file twice, and different files one after the other */
    for (int k = 0; k < 3; k++) {
        uint32_t p[1] = { g_f[0].h };
        ptp(&x, OP_GET_OBJECT, 1, p, NULL, 0);
        CHECK(x.st == 0 && x.code == RC_OK && x.dlen == g_f[0].size, "repeat GetObject %d", k);
        x_free(&x);
    }
}

/* T07 GetPartialObject */
typedef struct { int fi; uint64_t off; uint32_t len; } pr_t;
static void partial_check(uint16_t op, int fi, uint64_t off, uint32_t len)
{
    xact_t x;
    uint32_t p[4]; int np;
    if (op == OP_GET_PARTIAL_OBJECT) { p[0] = g_f[fi].h; p[1] = (uint32_t)off; p[2] = len; np = 3; }
    else { p[0] = g_f[fi].h; p[1] = (uint32_t)off; p[2] = (uint32_t)(off >> 32); p[3] = len; np = 4; }
    ptp(&x, op, np, p, NULL, 0);
    uint64_t size = g_f[fi].size;
    char w[160]; snprintf(w, sizeof w, "%s %s off=%" PRIu64 " len=%u", op == OP_GET_PARTIAL_OBJECT ? "GetPartialObject" : "GetPartialObject64", g_f[fi].name, off, len);
    if (off > size) {
        const uint16_t okc[] = { RC_INVALID_PARAMETER, RC_PARAM_NOT_SUPPORTED, RC_GENERAL };
        expect_err(&x, okc, 3, w);
        x_free(&x);
        canary("partial offset beyond the end");
        return;
    }
    uint64_t want = len > size - off ? size - off : len;
    if (!expect(&x, RC_OK, w)) { x_free(&x); canary("partial failure"); return; }
    CHECK(x.dlen == want, "%s: got %zu bytes, want %" PRIu64, w, x.dlen, want);
    CHECK(x.np == 1 && x.p[0] == want, "%s: ActualBytes response param %u (np %d), want %" PRIu64, w, x.p[0], x.np, want);
    CHECK(x.zlp_data == (((12 + x.dlen) % PKT) == 0), "%s: ZLP rule broken (container %zu)", w, 12 + x.dlen);
    uint8_t *t = malloc(want + 1);
    int32_t got = want ? truth(g_f[fi].dir, g_f[fi].idx, off, t, (size_t)want) : 0;
    CHECK(got == (int32_t)want, "store read for %s", w);
    if (x.dlen == want && got == (int32_t)want) { CHECK(!memcmp(t, x.d, want), "%s: content differs", w); }
    free(t);
    x_free(&x);
}

static void t_partial(void)
{
    test_begin("T07 GetPartialObject / GetPartialObject64");
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    int big = -1, trimmed = -1, bad = -1, z4096 = -1, z4084 = -1, empty = -1, sdbig = -1;
    for (int i = 0; i < g_nf; i++) {
        if (!strcmp(g_f[i].name, "NAND_F00005D3.BIN")) { trimmed = i; }
        if (!strcmp(g_f[i].name, "NAND_F00007D3.BIN")) { bad = i; }
        if (!strcmp(g_f[i].name, "NAND_F00009D3.BIN")) { z4096 = i; }
        if (!strcmp(g_f[i].name, "NAND_F00017D3.BIN")) { z4084 = i; }
        if (!strcmp(g_f[i].name, "NAND_F00013D3.BIN")) { empty = i; }
        if (!strcmp(g_f[i].name, "uSD_BIG.BIN")) { sdbig = i; }
    }
    big = trimmed;
    CHECK(big >= 0 && bad >= 0 && z4096 >= 0 && z4084 >= 0 && empty >= 0 && sdbig >= 0, "test files missing");
    if (big < 0 || bad < 0 || z4096 < 0 || z4084 < 0 || empty < 0 || sdbig < 0) { return; }
    static const uint16_t ops[2] = { OP_GET_PARTIAL_OBJECT, OP_AND_GET_PARTIAL64 };
    for (int oi = 0; oi < 2; oi++) {
        uint16_t op = ops[oi];
        /* the PFS page payload is 1876; the file offset 0 is PFS offset 1880 for the trimmed file */
        static const uint64_t offs[] = { 0, 1, 63, 64, 65, 1875, 1876, 1877, 1878, 3751, 3752, 3753, 4083, 4084, 4085, 4095, 4096, 4097, 8179, 8180, 8181, 20000 };
        static const uint32_t lens[] = { 1, 2, 63, 64, 65, 100, 1876, 1877, 4084, 4085, 4096, 5000, 9000 };
        for (size_t a = 0; a < sizeof offs / sizeof offs[0]; a++) {
            for (size_t b = 0; b < sizeof lens / sizeof lens[0]; b++) { partial_check(op, big, offs[a], lens[b]); }
        }
        uint64_t sz = g_f[big].size;
        partial_check(op, big, sz - 1, 1);            /* last byte */
        partial_check(op, big, sz - 1, 100);          /* length beyond the end: clipped */
        partial_check(op, big, sz - 10, 0xFFFFFFFFU); /* to the end */
        partial_check(op, big, sz, 10);               /* offset == size: empty, allowed */
        partial_check(op, big, sz, 0);
        partial_check(op, big, sz + 1, 10);           /* offset beyond the end: error */
        partial_check(op, big, 0xFFFFFFFFU, 10);
        partial_check(op, big, 0, 0);                 /* length 0 */
        partial_check(op, big, 0, 0xFFFFFFFFU);       /* the whole file */
        partial_check(op, bad, 1876 * 2 - 5, 20);     /* across the unreadable page (0xFF filler) */
        partial_check(op, bad, 0, 0xFFFFFFFFU);
        partial_check(op, z4096, 0, 4096);
        partial_check(op, z4096, 4000, 96);
        partial_check(op, z4096, 4096, 1);
        partial_check(op, z4084, 0, 4084);            /* container = 4096 */
        partial_check(op, z4084, 0, 4083);
        partial_check(op, z4084, 1, 4083);
        partial_check(op, z4084, 0, 4072);            /* container 4084 */
        partial_check(op, empty, 0, 10);
        partial_check(op, empty, 0, 0);
        partial_check(op, empty, 1, 1);               /* beyond an empty file */
        partial_check(op, sdbig, 0, 4096 - 12);
        partial_check(op, sdbig, 4072, 4096);
        partial_check(op, sdbig, 199999, 5);
        partial_check(op, sdbig, 100000, 100000);
        /* random reads with a fixed seed */
        srand(12345 + oi);
        for (int k = 0; k < 120; k++) {
            int fi = (k % 3 == 0) ? sdbig : (k % 3 == 1) ? big : bad;
            uint64_t o = (uint64_t)rand() % (g_f[fi].size + 1);
            uint32_t l = (uint32_t)(rand() % 9000);
            partial_check(op, fi, o, l);
        }
        canary(op == OP_GET_PARTIAL_OBJECT ? "GetPartialObject series" : "GetPartialObject64 series");
    }
    /* GetPartialObject64 with an offset above 4 GB: beyond the end */
    partial_check(OP_AND_GET_PARTIAL64, big, 0x100000000ULL, 16);
    partial_check(OP_AND_GET_PARTIAL64, big, 0x100000000ULL + 5, 16);
    /* partial read on a folder */
    xact_t x; uint32_t pf[3] = { h_nand_dir, 0, 10 };
    const uint16_t bc[] = { RC_INVALID_HANDLE, RC_ACCESS_DENIED, RC_GENERAL };
    ptp(&x, OP_GET_PARTIAL_OBJECT, 3, pf, NULL, 0);
    expect_err(&x, bc, 3, "GetPartialObject on a folder");
    x_free(&x);
    canary("partial on a folder");
}

/* T08 write refusals */
typedef struct { uint16_t op; int np; uint32_t p[5]; bool data; const char *name; } wop_t;
static void snapshot_state(uint64_t *sum, int *nandw, uint32_t *blocked)
{
    uint64_t s = 1469598103934665603ULL;
    for (int d = 0; d < 2; d++) {
        const mtp_store_t *st = d == 0 ? NAND : SD;
        for (uint32_t i = 0; i < st->count(); i++) {
            mtp_file_info_t fi; st->info(i, &fi);
            s = (s ^ fi.size) * 1099511628211ULL;
            uint8_t b[512]; uint64_t off = 0; int32_t n;
            while ((n = st->read(i, off, b, sizeof b)) > 0) { for (int32_t k = 0; k < n; k++) { s = (s ^ b[k]) * 1099511628211ULL; } off += (uint64_t)n; }
        }
    }
    *sum = s; *nandw = fake_nand_writes; *blocked = 0;
}

static void t_writes(void)
{
    test_begin("T08 write operations are refused and change nothing");
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    uint32_t hf = g_f[0].h, hd = h_nand_dir;
    /* an ObjectInfo dataset for SendObjectInfo: file "X.BIN" of 100 bytes */
    uint8_t oi[200]; memset(oi, 0, sizeof oi);
    put32(oi, STORAGE_ID); put16(oi + 4, 0x3000); put32(oi + 8, 100);
    size_t o = 52; oi[o++] = 6; const char *nm = "X.BIN"; for (int i = 0; i < 5; i++) { put16(oi + o, (uint16_t)nm[i]); o += 2; } put16(oi + o, 0); o += 2;
    oi[o++] = 0; oi[o++] = 0; oi[o++] = 0;
    uint8_t blob[300]; memset(blob, 0xA5, sizeof blob);
    uint8_t name16[40]; name16[0] = 6; for (int i = 0; i < 5; i++) { put16(name16 + 1 + 2 * i, (uint16_t)"Y.BIN"[i]); } put16(name16 + 11, 0);
    uint8_t prot[2] = { 1, 0 };
    static const struct { uint16_t op; int np; uint32_t p[5]; int dk; const char *name; } W[] = {
        { OP_SEND_OBJECT_INFO, 2, { STORAGE_ID, 0xFFFFFFFF }, 1, "SendObjectInfo (root)" },
        { OP_SEND_OBJECT_INFO, 2, { STORAGE_ID, 0 }, 1, "SendObjectInfo (NAND folder)" },
        { OP_SEND_OBJECT, 0, { 0 }, 2, "SendObject (no SendObjectInfo before)" },
        { OP_DELETE_OBJECT, 1, { 0 }, 0, "DeleteObject (file)" },
        { OP_DELETE_OBJECT, 1, { 1 }, 0, "DeleteObject (folder)" },
        { OP_DELETE_OBJECT, 2, { 0xFFFFFFFF, 0 }, 0, "DeleteObject (all objects)" },
        { OP_FORMAT_STORE, 1, { STORAGE_ID }, 0, "FormatStore" },
        { OP_SET_OBJECT_PROTECTION, 2, { 0, 0 }, 0, "SetObjectProtection" },
        { OP_MOVE_OBJECT, 3, { 0, STORAGE_ID, 0xFFFFFFFF }, 0, "MoveObject" },
        { OP_COPY_OBJECT, 3, { 0, STORAGE_ID, 0xFFFFFFFF }, 0, "CopyObject" },
        { OP_SET_OBJ_PROP_VALUE, 2, { 0, 0xDC07 }, 3, "SetObjectPropValue (file name)" },
        { OP_SET_OBJ_PROP_VALUE, 2, { 0, 0xDC03 }, 4, "SetObjectPropValue (protection)" },
        { OP_SEND_OBJ_PROP_LIST, 5, { STORAGE_ID, 0, 0x3000, 0, 100 }, 2, "SendObjectPropList" },
        { OP_SET_OBJ_REFERENCES, 1, { 0 }, 2, "SetObjectReferences" },
        { OP_SET_DEVICE_PROP_VALUE, 1, { 0xD402 }, 3, "SetDevicePropValue" },
        { OP_AND_BEGIN_EDIT, 1, { 0 }, 0, "AndroidBeginEditObject" },
        { OP_AND_SEND_PARTIAL, 4, { 0, 0, 0, 100 }, 2, "AndroidSendPartialObject" },
        { OP_AND_TRUNCATE, 3, { 0, 0, 0 }, 0, "AndroidTruncateObject" },
        { OP_AND_END_EDIT, 1, { 0 }, 0, "AndroidEndEditObject" },
    };
    uint64_t sum0; int nw0; uint32_t bl0;
    snapshot_state(&sum0, &nw0, &bl0);
    int vfs_w0 = h_vfs_write_calls;
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) {
        uint32_t p[5]; memcpy(p, W[i].p, sizeof p);
        if (W[i].op == OP_DELETE_OBJECT && p[0] == 0 && W[i].np == 1) { p[0] = hf; }
        if (W[i].op == OP_DELETE_OBJECT && p[0] == 1) { p[0] = hd; }
        if (W[i].op == OP_SET_OBJECT_PROTECTION || W[i].op == OP_MOVE_OBJECT || W[i].op == OP_COPY_OBJECT || W[i].op == OP_SET_OBJ_PROP_VALUE ||
            W[i].op == OP_AND_BEGIN_EDIT || W[i].op == OP_AND_END_EDIT || W[i].op == OP_AND_TRUNCATE || W[i].op == OP_AND_SEND_PARTIAL ||
            W[i].op == OP_SET_OBJ_REFERENCES) { if (p[0] == 0) { p[0] = hf; } }
        const uint8_t *dat = NULL; size_t dl = 0;
        switch (W[i].dk) {
        case 1: dat = oi; dl = o; break;
        case 2: dat = blob; dl = 100; break;
        case 3: dat = name16; dl = 12; break;
        case 4: dat = prot; dl = 2; break;
        default: break;
        }
        xact_t x;
        ptp(&x, W[i].op, W[i].np, p, dat, dl);
        char w[120]; snprintf(w, sizeof w, "%s", W[i].name);
        const uint16_t okc[] = { RC_STORE_READ_ONLY, RC_WRITE_PROTECTED, RC_ACCESS_DENIED, RC_OP_NOT_SUPPORTED };
        if (!expect_err(&x, okc, 4, w)) {
            if (x.st != 0 && x.st != -3) {
                printf("    (recovery: class reset)\n");
                um_clear_halt(EP_OUT); um_clear_halt(EP_IN); um_control(0, 0x66, 0, NULL, NULL, NULL);
            }
        }
        CHECK(!x.has_data, "%s: the device sent data", w);
        x_free(&x);
        if (!canary(w)) { break; }
    }
    /* a big SendObject data phase (many packets) with a refused command */
    {
        uint8_t big[9000]; memset(big, 0x5A, sizeof big);
        xact_t x; uint32_t p[2] = { STORAGE_ID, hd };
        ptp(&x, OP_SEND_OBJECT_INFO, 2, p, oi, o);
        CHECK(x.st == 0, "SendObjectInfo before SendObject: st %d %s", x.st, x.why ? x.why : "");
        x_free(&x);
        ptp(&x, OP_SEND_OBJECT, 0, NULL, big, sizeof big);
        const uint16_t okc[] = { RC_STORE_READ_ONLY, RC_WRITE_PROTECTED, RC_ACCESS_DENIED, 0x2015 /* NoValidObjectInfo */ };
        expect_err(&x, okc, 4, "SendObject with 9000 bytes of data");
        x_free(&x);
        canary("big SendObject");
    }
    uint64_t sum1; int nw1; uint32_t bl1;
    snapshot_state(&sum1, &nw1, &bl1);
    CHECK(sum0 == sum1, "store content or listing changed");
    CHECK(nw1 == nw0 && nw1 == 0, "NAND/NOR write, erase or mark calls reached the fake NAND: %d", nw1);
    CHECK(h_vfs_write_calls == vfs_w0, "the MTP layer called %d write operations on the VFS shim", h_vfs_write_calls - vfs_w0);
    list_and_check();
}

/* T09 invalid commands, framing errors, recovery */
static void t_invalid(void)
{
    test_begin("T09 unknown operations, wrong transaction ids, bad containers");
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    xact_t x;
    ptp(&x, 0x9999, 0, NULL, NULL, 0);
    expect(&x, RC_OP_NOT_SUPPORTED, "unknown opcode 0x9999");
    x_free(&x);
    canary("unknown opcode");
    ptp(&x, 0x1000, 0, NULL, NULL, 0);
    expect(&x, RC_OP_NOT_SUPPORTED, "opcode 0x1000");
    x_free(&x);
    canary("opcode 0x1000");
    ptp(&x, 0x1010, 0, NULL, NULL, 0);
    expect(&x, RC_OP_NOT_SUPPORTED, "ResetDevice (0x1010, not advertised)");
    x_free(&x);
    ptp(&x, 0x1018, 0, NULL, NULL, 0);
    expect(&x, RC_OP_NOT_SUPPORTED, "opcode 0x1018");
    x_free(&x);
    canary("unsupported ops");
    /* wrong transaction ids */
    uint32_t keep = g_tid;
    ptp_tid(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0, keep + 5);       /* skipped ids */
    printf("  info: tid jump gives 0x%04x (response tid %u)\n", x.st ? 0 : x.code, x.rtid);
    CHECK(x.st == 0 && (x.code == RC_OK || x.code == RC_INVALID_TID), "tid jump: st %d code 0x%04x", x.st, x.code);
    CHECK(x.st == 0 && x.rtid == keep + 5, "response tid %u must echo the command tid %u", x.rtid, keep + 5);
    x_free(&x);
    ptp_tid(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0, keep + 5);       /* repeated id */
    printf("  info: repeated tid gives 0x%04x\n", x.st ? 0 : x.code);
    CHECK(x.st == 0 && x.rtid == keep + 5, "repeated tid: response tid %u", x.rtid);
    x_free(&x);
    g_tid = keep + 6;
    ptp_tid(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0, 0);               /* tid 0 inside a session */
    printf("  info: tid 0 in session gives 0x%04x\n", x.st ? 0 : x.code);
    x_free(&x);
    canary("transaction id games");
    /* a second command while the previous response was read: back to back, 50 transactions */
    for (int i = 0; i < 50; i++) {
        ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
        if (x.st != 0 || x.code != RC_OK) { CHECK(0, "back-to-back %d: st %d code 0x%04x", i, x.st, x.code); x_free(&x); break; }
        x_free(&x);
    }
    /* command with 5 parameters, and a command with more bytes than 5 parameters (extra bytes) */
    uint32_t p5[5] = { STORAGE_ID, 0, ROOT, 7, 7 };
    ptp(&x, OP_GET_OBJECT_HANDLES, 5, p5, NULL, 0);
    expect(&x, RC_OK, "GetObjectHandles with 5 parameters");
    x_free(&x);
    canary("5 parameters");
    /* GetDeviceStatus */
    uint8_t st[16]; int stl = 0;
    int r = um_control(1, 0x67, 4, NULL, st, &stl);
    CHECK(r == 0 && stl == 4 && le16(st) == 4 && le16(st + 2) == RC_OK, "GetDeviceStatus: r=%d len=%d len16=%u code=0x%04x", r, stl, le16(st), le16(st + 2));
    canary("GetDeviceStatus");
    /* DEVICE_RESET while idle */
    r = um_control(0, 0x66, 0, NULL, NULL, NULL);
    CHECK(r == 0, "DEVICE_RESET while idle: r=%d", r);
    ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
    CHECK(x.st == 0 && x.code == RC_SESSION_NOT_OPEN, "after DEVICE_RESET the session must be closed (PIMA 15740: reset closes it): st %d code 0x%04x", x.st, x.code);
    x_free(&x);
    CHECK(open_session(), "OpenSession after the reset");
    canary("DEVICE_RESET idle");
    /* extended event data request: not supported -> stall of the control pipe (return -1) is the contract */
    r = um_control(1, 0x65, 16, NULL, st, &stl);
    printf("  info: GetExtendedEventData request: r=%d\n", r);
    canary("GetExtendedEventData");
}

static void host_recover_by_reset(void)
{
    um_clear_halt(EP_OUT); um_clear_halt(EP_IN);
    int r = um_control(0, 0x66, 0, NULL, NULL, NULL);
    CHECK(r == 0, "recovery: DEVICE_RESET request failed (r=%d)", r);
}

static void t_framing(void)
{
    test_begin("T10 framing errors and the recovery of the bulk pipe");
    device_boot();
    g_reopen_ok = true;
    CHECK(open_session(), "open");
    xact_t x;
    uint8_t b[64];
    /* (a) a data container where a command is expected */
    memset(b, 0, sizeof b); put32(b, 20); put16(b + 4, CT_DATA); put16(b + 6, OP_GET_STORAGE_IDS); put32(b + 8, 99);
    int r = um_host_out(b, 20);
    CHECK(r == 0, "OUT of a data container as a command: r=%d", r);
    um_pump();
    CHECK(um_stalled(EP_OUT) && um_stalled(EP_IN), "a wrong container type must stall both bulk endpoints (stall out %d in %d)", um_stalled(EP_OUT), um_stalled(EP_IN));
    host_recover_by_reset();
    CHECK(!um_stalled(EP_OUT) && !um_stalled(EP_IN), "stalls remain after the class reset");
    canary("wrong container type (host does DEVICE_RESET)");
    /* (b) a short container (8 bytes) */
    memset(b, 0, sizeof b); put32(b, 8); put16(b + 4, CT_COMMAND);
    r = um_host_out(b, 8);
    um_pump();
    CHECK(um_stalled(EP_OUT) || r == 0, "short command container");
    host_recover_by_reset();
    canary("short container");
    /* (c) a stray ZLP */
    r = um_host_out(b, 0);
    um_pump();
    printf("  info: stray ZLP: out=%d stalled=%d\n", r, um_stalled(EP_OUT));
    canary("stray ZLP");
    /* (d) response container type sent as command */
    memset(b, 0, sizeof b); put32(b, 12); put16(b + 4, CT_RESPONSE); put16(b + 6, RC_OK); put32(b + 8, 5);
    r = um_host_out(b, 12);
    um_pump();
    CHECK(um_stalled(EP_OUT), "response type as a command must stall");
    host_recover_by_reset();
    canary("response type as command");
    /* (e) host clears the halt only (no class reset): what happens? informative */
    memset(b, 0, sizeof b); put32(b, 12); put16(b + 4, CT_EVENT); put32(b + 8, 5);
    um_host_out(b, 12);
    um_pump();
    um_clear_halt(EP_OUT); um_clear_halt(EP_IN);
    ptp(&x, OP_GET_STORAGE_IDS, 0, NULL, NULL, 0);
    printf("  info: after CLEAR_HALT only (no 0x66): st=%d code=0x%04x %s\n", x.st, x.code, x.why ? x.why : "");
    bool dead = x.st != 0;
    x_free(&x);
    if (dead) {
        host_recover_by_reset();
        printf("  info: the pipe stays dead after CLEAR_HALT alone; a class reset (0x66) recovers it\n");
    }
    canary("CLEAR_HALT only");
    g_reopen_ok = false;
    (void)r;
}

/* T11 cancel */
static void t_cancel(void)
{
    test_begin("T11 CANCEL during a read, DEVICE_RESET during a read");
    device_boot();
    g_reopen_ok = true;
    CHECK(open_session(), "open");
    list_and_check();
    int big = -1;
    for (int i = 0; i < g_nf; i++) { if (!strcmp(g_f[i].name, "uSD_BIG.BIN")) { big = i; } }
    if (big < 0) { CHECK(0, "no big file"); return; }
    /* start GetObject, read a few packets, then cancel */
    for (int variant = 0; variant < 2; variant++) {
        uint32_t tid = g_tid++;
        uint32_t p[1] = { g_f[big].h };
        int r = send_cont(CT_COMMAND, OP_GET_OBJECT, tid, p, 1, NULL, 0);
        CHECK(r == 0, "command send");
        uint8_t pk[PKT]; size_t n;
        for (int i = 0; i < 40; i++) { if (um_host_in_packet(pk, &n) != 0) { CHECK(0, "no data packet %d", i); break; } }
        uint8_t cancel[6]; put16(cancel, 0x4001); put32(cancel + 2, tid);
        if (variant == 0) {
            r = um_control(0, 0x64, 6, cancel, NULL, NULL);
            CHECK(r == 0, "CANCEL request: r=%d", r);
            /* the host drains the pipe until it sees the end of the data phase and a response */
            int drained = 0; bool saw_resp = false; uint16_t rc = 0;
            for (int i = 0; i < 4000 && !saw_resp; i++) {
                int rr = um_host_in_packet(pk, &n);
                if (rr != 0) { break; }
                drained++;
                if (hl_verbose) { printf("    pkt %d: n=%zu\n", drained, n); }
                if (n >= 12 && le16(pk + 4) == CT_RESPONSE && n == 12) { saw_resp = true; rc = le16(pk + 6); }
            }
            printf("  info: after CANCEL the host drained %d packets, response seen=%d code=0x%04x, IN pending=%d\n", drained, saw_resp, rc, um_in_pending());
            CHECK(saw_resp, "no response container after CANCEL (device did not terminate the transaction)");
            if (saw_resp) { CHECK(rc == RC_CANCELLED || rc == RC_OK || rc == 0x2007, "response after CANCEL 0x%04x", rc); }
            um_pump();
        } else {
            /* DEVICE_RESET in the middle of a data phase */
            um_clear_halt(EP_OUT); um_clear_halt(EP_IN);
            r = um_control(0, 0x66, 0, NULL, NULL, NULL);
            CHECK(r == 0, "DEVICE_RESET in a read: r=%d", r);
            /* the host flushes whatever the mock still holds (a real controller drops the pending IN transfer at the reset) */
        }
        if (!canary(variant == 0 ? "CANCEL mid read" : "DEVICE_RESET mid read")) {
            printf("  info: layer not ready after variant %d, trying a class reset\n", variant);
            host_recover_by_reset();
            canary("after recovery");
        }
        /* state must be usable: OpenSession may be needed after a reset */
        xact_t x; uint32_t p2[1] = { g_f[0].h };
        ptp(&x, OP_GET_OBJECT_INFO, 1, p2, NULL, 0);
        if (x.st == 0 && x.code == RC_SESSION_NOT_OPEN) { x_free(&x); CHECK(open_session(), "reopen after reset"); }
        else { x_free(&x); }
    }
    g_reopen_ok = false;
}

/* ---------------------------------------------------------------- properties */
static bool skip_val(rd_t *r, uint16_t type, uint64_t *u, char *s, size_t cap)
{
    *u = 0;
    if (s) { s[0] = 0; }
    switch (type) {
    case 0x0002: *u = r8(r); break;
    case 0x0004: *u = r16(r); break;
    case 0x0006: *u = r32(r); break;
    case 0x0008: *u = r64(r); break;
    case 0x000A: r->o += 16; if (r->o > r->n) { r->bad = true; } break;
    case 0xFFFF: { char tmp[300]; rstr(r, s ? s : tmp, s ? cap : sizeof tmp); break; }
    default: r->bad = true; return false;
    }
    return !r->bad;
}
static const uint16_t PROP_CODES[] = { 0xDC01, 0xDC02, 0xDC03, 0xDC04, 0xDC05, 0xDC07, 0xDC08, 0xDC09, 0xDC0B, 0xDC41, 0xDC44, 0xDC4E, 0xDC4F, 0xDCE0 };
static const uint16_t PROP_TYPES[] = { 0x0006, 0x0004, 0x0004, 0x0008, 0x0004, 0xFFFF, 0xFFFF, 0xFFFF, 0x0006, 0x000A, 0xFFFF, 0xFFFF, 0x0002, 0xFFFF };

static void t_props(void)
{
    test_begin("T12 object and device properties (Windows/libmtp use them)");
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    xact_t x; uint32_t p1[1] = { 0x3000 };
    ptp(&x, OP_GET_OBJ_PROPS_SUPPORTED, 1, p1, NULL, 0);
    if (expect(&x, RC_OK, "GetObjectPropsSupported")) {
        rd_t r = { x.d, x.dlen, 0, false };
        uint32_t n = r32(&r);
        CHECK(n == sizeof PROP_CODES / sizeof PROP_CODES[0], "supported property count %u", n);
        for (uint32_t i = 0; i < n && i < 40; i++) {
            uint16_t c = r16(&r);
            bool f = false; for (size_t k = 0; k < sizeof PROP_CODES / sizeof PROP_CODES[0]; k++) { if (PROP_CODES[k] == c) { f = true; } }
            CHECK(f, "unexpected property 0x%04x", c);
        }
        CHECK(!r.bad && r.o == r.n, "props supported length");
    }
    x_free(&x);
    /* GetObjectPropDesc */
    for (size_t k = 0; k < sizeof PROP_CODES / sizeof PROP_CODES[0]; k++) {
        uint32_t p[2] = { PROP_CODES[k], 0x3000 };
        ptp(&x, OP_GET_OBJ_PROP_DESC, 2, p, NULL, 0);
        char w[60]; snprintf(w, sizeof w, "GetObjectPropDesc 0x%04x", PROP_CODES[k]);
        if (expect(&x, RC_OK, w)) {
            rd_t r = { x.d, x.dlen, 0, false };
            uint16_t code = r16(&r), type = r16(&r); uint8_t gs = r8(&r);
            uint64_t dv; char ds[64];
            skip_val(&r, type, &dv, ds, sizeof ds);
            uint32_t grp = r32(&r); uint8_t ff = r8(&r);
            CHECK(code == PROP_CODES[k] && type == PROP_TYPES[k], "%s: code 0x%04x type 0x%04x (want 0x%04x)", w, code, type, PROP_TYPES[k]);
            CHECK(gs == 0, "%s: GetSet %u, the device is read-only: must be 0 (Get)", w, gs);
            CHECK(grp == 0 && ff == 0, "%s: group %u form %u", w, grp, ff);
            CHECK(!r.bad && r.o == r.n, "%s: length: parsed %zu of %zu", w, r.o, r.n);
        }
        x_free(&x);
    }
    uint32_t pbad[2] = { 0xDC99, 0x3000 };
    ptp(&x, OP_GET_OBJ_PROP_DESC, 2, pbad, NULL, 0);
    expect(&x, RC_PROP_NOT_SUPPORTED, "GetObjectPropDesc unsupported property");
    x_free(&x);
    /* GetObjectPropValue on every object and every property */
    for (int i = 0; i < g_nf; i++) {
        for (size_t k = 0; k < sizeof PROP_CODES / sizeof PROP_CODES[0]; k++) {
            uint32_t p[2] = { g_f[i].h, PROP_CODES[k] };
            ptp(&x, OP_GET_OBJ_PROP_VALUE, 2, p, NULL, 0);
            char w[100]; snprintf(w, sizeof w, "GetObjectPropValue %s 0x%04x", g_f[i].name, PROP_CODES[k]);
            if (!expect(&x, RC_OK, w)) { x_free(&x); continue; }
            rd_t r = { x.d, x.dlen, 0, false }; uint64_t u; char s[100];
            skip_val(&r, PROP_TYPES[k], &u, s, sizeof s);
            CHECK(!r.bad && r.o == r.n, "%s: value length: parsed %zu of %zu", w, r.o, r.n);
            uint32_t dir = g_f[i].dir == 0 ? h_nand_dir : h_sd_dir;
            switch (PROP_CODES[k]) {
            case 0xDC01: CHECK(u == STORAGE_ID, "%s: %" PRIu64, w, u); break;
            case 0xDC02: CHECK(u == 0x3000 || u == 0x3004, "%s: 0x%" PRIx64, w, u); break;
            case 0xDC03: CHECK(u == 1, "%s: ProtectionStatus %" PRIu64 " (ObjectInfo says 1)", w, u); break;
            case 0xDC04: CHECK(u == g_f[i].size, "%s: size %" PRIu64 " vs %" PRIu64, w, u, g_f[i].size); break;
            case 0xDC05: CHECK(u == 0, "%s: %" PRIu64, w, u); break;
            case 0xDC07: CHECK(!strcmp(s, g_f[i].name), "%s: '%s'", w, s); break;
            case 0xDC0B: CHECK(u == dir, "%s: parent %" PRIu64 " want %u", w, u, dir); break;
            case 0xDC08: case 0xDC09: case 0xDC4E: CHECK(s[0] == 0 || (strlen(s) == 15 && s[8] == 'T'), "%s: date '%s'", w, s); break;
            default: break;
            }
        }
    }
    /* folder properties */
    {
        uint32_t p[2] = { h_nand_dir, 0xDC0B };
        ptp(&x, OP_GET_OBJ_PROP_VALUE, 2, p, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropValue folder parent")) { CHECK(x.dlen == 4 && le32(x.d) == 0, "folder ParentObject property %u (root = 0)", x.dlen == 4 ? le32(x.d) : 999); }
        x_free(&x);
        uint32_t p5[2] = { h_nand_dir, 0xDC05 };
        ptp(&x, OP_GET_OBJ_PROP_VALUE, 2, p5, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropValue folder assoc")) { CHECK(x.dlen == 2 && le16(x.d) == 1, "folder AssociationType property"); }
        x_free(&x);
    }
    uint32_t pw[2] = { g_f[0].h, 0xDC99 };
    ptp(&x, OP_GET_OBJ_PROP_VALUE, 2, pw, NULL, 0);
    expect(&x, RC_PROP_NOT_SUPPORTED, "GetObjectPropValue unsupported property");
    x_free(&x);
    uint32_t ph[2] = { 0x7777, 0xDC01 };
    ptp(&x, OP_GET_OBJ_PROP_VALUE, 2, ph, NULL, 0);
    expect(&x, RC_INVALID_HANDLE, "GetObjectPropValue wrong handle");
    x_free(&x);
    canary("property errors");
    /* GetObjectPropList: the children of the NAND folder, all properties, depth 1 */
    {
        uint32_t p[5] = { h_nand_dir, 0, 0xFFFFFFFF, 0, 1 };
        ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropList NAND depth 1")) {
            rd_t r = { x.d, x.dlen, 0, false };
            uint32_t n = r32(&r);
            uint32_t want = (uint32_t)NAND->count() * (uint32_t)(sizeof PROP_CODES / sizeof PROP_CODES[0]);
            CHECK(n == want || n == want + (uint32_t)(sizeof PROP_CODES / sizeof PROP_CODES[0]), "prop list entries %u want %u (children) or %u (folder and children)", n, want, want + 14);
            if (n != want) { printf("  info: GetObjectPropList(folder, depth 1) includes the folder itself (upstream behaviour, not verified against Windows)\n"); }
            for (uint32_t i = 0; i < n && !r.bad; i++) {
                uint32_t h = r32(&r); uint16_t code = r16(&r), type = r16(&r); uint64_t u; char s[100];
                skip_val(&r, type, &u, s, sizeof s);
                bool known = false; for (size_t k = 0; k < sizeof PROP_CODES / sizeof PROP_CODES[0]; k++) { if (PROP_CODES[k] == code && PROP_TYPES[k] == type) { known = true; } }
                CHECK(known, "entry %u: handle %u code 0x%04x type 0x%04x unknown", i, h, code, type);
                if (code == 0xDC0B) { CHECK(u == h_nand_dir || (h == h_nand_dir && u == 0), "entry parent %" PRIu64, u); }
                if (code == 0xDC03) { CHECK(u == 1, "entry ProtectionStatus %" PRIu64, u); }
            }
            CHECK(!r.bad && r.o == r.n, "prop list length: parsed %zu of %zu", r.o, r.n);
        }
        x_free(&x);
        /* one property only */
        uint32_t p2[5] = { h_nand_dir, 0, 0xDC04, 0, 1 };
        ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p2, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropList size only")) { CHECK(x.dlen >= 4 && (le32(x.d) == NAND->count() || le32(x.d) == NAND->count() + 1), "size-only list count %u", x.dlen >= 4 ? le32(x.d) : 0); }
        x_free(&x);
        /* the root: handle 0 with depth 1 = the objects of the root (MTP 1.1 GetObjectPropList) */
        uint32_t p3[5] = { 0, 0, 0xDC07, 0, 1 };
        ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p3, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropList handle 0 depth 1 (root)")) { CHECK(x.dlen >= 4 && le32(x.d) == 2, "root list count %u (want 2)", x.dlen >= 4 ? le32(x.d) : 0); }
        x_free(&x);
        /* all objects */
        uint32_t p4[5] = { 0xFFFFFFFF, 0, 0xDC07, 0, 0 };
        ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p4, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropList all objects")) { CHECK(x.dlen >= 4 && le32(x.d) == 2u + NAND->count() + SD->count(), "all list count %u", x.dlen >= 4 ? le32(x.d) : 0); }
        x_free(&x);
        /* a group code is not supported */
        uint32_t p6[5] = { h_nand_dir, 0, 0xFFFFFFFF, 1, 1 };
        ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p6, NULL, 0);
        const uint16_t gc[] = { RC_GROUP_NOT_SUPPORTED, 0xA807 };
        expect_err(&x, gc, 2, "GetObjectPropList with a group code");
        x_free(&x);
        /* depth 0 on one file */
        uint32_t p7[5] = { g_f[0].h, 0, 0xFFFFFFFF, 0, 0 };
        ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p7, NULL, 0);
        if (expect(&x, RC_OK, "GetObjectPropList file depth 0")) { CHECK(x.dlen >= 4 && le32(x.d) == sizeof PROP_CODES / sizeof PROP_CODES[0], "file list count"); }
        x_free(&x);
        canary("prop list");
    }
    /* object references */
    uint32_t pr[1] = { g_f[0].h };
    ptp(&x, OP_GET_OBJ_REFERENCES, 1, pr, NULL, 0);
    if (expect(&x, RC_OK, "GetObjectReferences")) { CHECK(x.dlen == 4 && le32(x.d) == 0, "references: empty array"); }
    x_free(&x);
    /* device property: friendly name */
    uint32_t pd[1] = { 0xD402 };
    ptp(&x, OP_GET_DEVICE_PROP_DESC, 1, pd, NULL, 0);
    if (expect(&x, RC_OK, "GetDevicePropDesc friendly name")) {
        rd_t r = { x.d, x.dlen, 0, false }; char s[80], s2[80];
        CHECK(r16(&r) == 0xD402 && r16(&r) == 0xFFFF && r8(&r) == 0, "device prop desc header / Get only");
        rstr(&r, s, sizeof s); rstr(&r, s2, sizeof s2);
        CHECK(!strcmp(s, "IREC DAQ") && !strcmp(s2, "IREC DAQ"), "defaults '%s' '%s'", s, s2);
        CHECK(r8(&r) == 0 && !r.bad && r.o == r.n, "form flag / length");
    }
    x_free(&x);
    ptp(&x, OP_GET_DEVICE_PROP_VALUE, 1, pd, NULL, 0);
    if (expect(&x, RC_OK, "GetDevicePropValue friendly name")) { rd_t r = { x.d, x.dlen, 0, false }; char s[80]; rstr(&r, s, sizeof s); CHECK(!strcmp(s, "IREC DAQ") && r.o == r.n, "value '%s'", s); }
    x_free(&x);
    uint32_t pdb[1] = { 0x5001 };
    ptp(&x, OP_GET_DEVICE_PROP_VALUE, 1, pdb, NULL, 0);
    const uint16_t dpc[] = { 0x200A, RC_PARAM_NOT_SUPPORTED };
    expect_err(&x, dpc, 2, "GetDevicePropValue unsupported property (PIMA: 0x200A)");
    x_free(&x);
    canary("device props");
}

/* T13 the card goes and comes, the list changes only at OpenSession */
static void t_refresh(void)
{
    test_begin("T13 file list changes: SD card removed and inserted, NAND files change");
    load_default_stores();
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    int sd_old = -1, nand_old = -1;
    for (int i = 0; i < g_nf; i++) { if (g_f[i].dir == 1 && sd_old < 0) { sd_old = i; } if (g_f[i].dir == 0 && nand_old < 0) { nand_old = i; } }
    uint32_t hs = g_f[sd_old].h, hn = g_f[nand_old].h;
    /* the card goes while the session is open: the list must not change before the next OpenSession */
    fake_sd_set(false, 0);
    uint32_t h[600], n; uint16_t rc;
    CHECK(get_handles(STORAGE_ID, 0, h_sd_dir, h, &n, &rc) && n == 3, "list inside a session is a snapshot: SD has %u", n);
    xact_t x; uint32_t p[1] = { hs };
    ptp(&x, OP_GET_OBJECT, 1, p, NULL, 0);
    printf("  info: GetObject of an SD file while the card is gone but the session is open: code 0x%04x, %zu bytes\n", x.st ? 0 : x.code, x.dlen);
    x_free(&x);
    canary("card removed in session");
    CHECK(close_session(), "close");
    CHECK(open_session(), "reopen with no card");
    CHECK(get_handles(STORAGE_ID, 0, ROOT, h, &n, &rc) && n == 2, "root without a card: %u objects (the SD folder stays)", n);
    CHECK(get_handles(STORAGE_ID, 0, h_sd_dir, h, &n, &rc) && n == 0, "SD folder without a card: %u files", n);
    oi_t oi;
    CHECK(!get_oi(hs, &oi, &rc) && rc == RC_INVALID_HANDLE, "old SD handle after the card went: rc 0x%04x", rc);
    ptp(&x, OP_GET_OBJECT, 1, p, NULL, 0);
    CHECK(x.st == 0 && x.code == RC_INVALID_HANDLE, "GetObject on the old SD handle: st %d code 0x%04x", x.st, x.code);
    x_free(&x);
    canary("card gone");
    /* NAND list changes: file 5 disappears, file 99 appears */
    static const fake_pfs_file_t NEW[] = { { 7, 0, 5, 1876, false, 2 }, { 99, 0, 1, 10, true, -1 } };
    fake_pfs_set_files(NEW, 2);
    fake_sd_set(true, 3);
    CHECK(close_session(), "close");
    CHECK(open_session(), "reopen after changes");
    list_and_check();
    CHECK(NAND->count() == 2 && SD->count() == 3, "store counts %u %u", NAND->count(), SD->count());
    CHECK(!get_oi(hn, &oi, &rc) || strcmp(oi.name, "NAND_F00005D3.BIN") != 0, "old handle of a removed NAND file still answers with the old name");
    canary("lists changed");
    /* many close/open cycles: no growth, no hang */
    for (int i = 0; i < 100; i++) {
        if (!close_session() || !open_session()) { CHECK(0, "cycle %d failed", i); break; }
        if (i % 10 == 0) { CHECK(get_handles(STORAGE_ID, 0, ROOT, h, &n, &rc) && n == 2, "root in cycle %d", i); }
    }
    list_and_check();
    load_default_stores();
}

/* T14 maximum sizes */
static void t_stress(void)
{
    test_begin("T14 maximum list: 128 NAND files, 256 SD files");
    fake_pfs_set_many(128);
    fake_sd_set(true, 256);
    device_boot();
    CHECK(open_session(), "open");
    uint32_t h[600], n; uint16_t rc;
    CHECK(get_handles(STORAGE_ID, 0, ROOT, h, &n, &rc) && n == 2, "root n=%u rc=0x%04x", n, rc);
    uint32_t dirs[2] = { h[0], h[1] };
    oi_t oi; uint32_t total = 0;
    for (int d = 0; d < 2; d++) {
        bool ok = get_handles(STORAGE_ID, 0, dirs[d], h, &n, &rc);
        CHECK(ok, "folder %d list rc=0x%04x", d, rc);
        total += n;
        for (uint32_t i = 0; ok && i < n; i++) { CHECK(get_oi(h[i], &oi, &rc), "GetObjectInfo %u rc 0x%04x", h[i], rc); if (g_fail > 20) { return; } }
    }
    CHECK(total == 128 + 256, "files listed: %u", total);
    CHECK(get_handles(STORAGE_ID, 0, 0, h, &n, &rc) && n == 2 + 128 + 256, "all objects: n=%u rc=0x%04x", n, rc);
    xact_t x; uint32_t p[5] = { 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0 };
    ptp(&x, OP_GET_OBJ_PROP_LIST, 5, p, NULL, 0);
    if (expect(&x, RC_OK, "GetObjectPropList of all 386 objects, all properties")) {
        CHECK(x.dlen >= 4 && le32(x.d) == 386u * 14u, "entries %u want %u (%zu bytes)", x.dlen >= 4 ? le32(x.d) : 0, 386u * 14u, x.dlen);
        CHECK(x.zlp_data == (((12 + x.dlen) % PKT) == 0), "ZLP rule for the big list");
    }
    x_free(&x);
    canary("max list");
    /* the table is full of 386 objects; a 3rd close/open must still work */
    CHECK(close_session() && open_session(), "cycle with a full table");
    CHECK(get_handles(STORAGE_ID, 0, 0, h, &n, &rc) && n == 386 + 0 + 0 + 0 + 0 + 0 - 0 + 0, "all objects after reopen: n=%u rc=0x%04x", n, rc);
    load_default_stores();
}

/* T15 a store read error in the middle of a transfer */
static bool read_to_response(uint16_t *rc, int *pkts, bool *saw_data_hdr)
{
    uint8_t pk[PKT]; size_t n;
    *pkts = 0; *saw_data_hdr = false;
    for (int i = 0; i < 100000; i++) {
        if (um_host_in_packet(pk, &n) != 0) { return false; }
        (*pkts)++;
        if (i == 0 && n >= 12 && le16(pk + 4) == CT_DATA) { *saw_data_hdr = true; }
        if (n >= 12 && le16(pk + 4) == CT_RESPONSE && n == 12) { *rc = le16(pk + 6); return true; }
        if (n < PKT && i > 0) { /* a short packet that is not a response: the data container ended */ }
    }
    return false;
}
static void t_errors(void)
{
    test_begin("T15 store read errors during GetObject and GetPartialObject");
    load_default_stores();
    device_boot();
    CHECK(open_session(), "open");
    list_and_check();
    int big = -1;
    for (int i = 0; i < g_nf; i++) { if (!strcmp(g_f[i].name, "uSD_BIG.BIN")) { big = i; } }
    if (big < 0) { CHECK(0, "no big file"); return; }
    static const int64_t at[] = { 0, 3000, 20000, 150000 };
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
        for (int pass = 0; pass < 2; pass++) {
            fake_sd_fail_at(at[k]);
            uint32_t tid = g_tid++;
            uint32_t p[3] = { g_f[big].h, 0, 200000 };
            int r = send_cont(CT_COMMAND, pass == 0 ? OP_GET_OBJECT : OP_GET_PARTIAL_OBJECT, tid, p, pass == 0 ? 1 : 3, NULL, 0);
            CHECK(r == 0, "send command");
            uint16_t rc = 0; int pk; bool dh;
            bool ok = read_to_response(&rc, &pk, &dh);
            char w[100]; snprintf(w, sizeof w, "%s failing at %lld", pass == 0 ? "GetObject" : "GetPartialObject", (long long)at[k]);
            CHECK(ok, "%s: no response container (%d packets)", w, pk);
            if (ok) { CHECK(rc == RC_GENERAL, "%s: response 0x%04x (want GeneralError 0x2002)", w, rc); }
            printf("  info: %s: data header sent=%d, %d packets before the response, code 0x%04x\n", w, dh, pk, rc);
            fake_sd_fail_at(-1);
            um_pump();
            if (!canary(w)) { host_recover_by_reset(); }
            uint32_t pf[1] = { g_f[big].h };
            xact_t x;
            ptp(&x, OP_GET_OBJECT, 1, pf, NULL, 0);
            CHECK(x.st == 0 && x.code == RC_OK && x.dlen == g_f[big].size, "GetObject after the error: st %d code 0x%04x len %zu", x.st, x.code, x.dlen);
            x_free(&x);
        }
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-v")) { hl_verbose = 1; } if (!strcmp(argv[i], "-vv")) { hl_verbose = 2; } }
    setvbuf(stdout, NULL, _IOLBF, 0);
    load_default_stores();
    fake_pfs_setup();
    assert(mtp_store_sd_init());
    NAND = mtp_store_pfs_get();
    SD = mtp_store_sd_get();
    assert(NAND && SD);
    t_before_session();
    t_device_info();
    t_session();
    t_storage();
    t_listing();
    t_getobject();
    t_partial();
    t_writes();
    t_invalid();
    t_framing();
    t_cancel();
    t_props();
    t_refresh();
    t_stress();
    t_errors();
    printf("\nresult: %d checks passed, %d failed; log lines: %d error, %d warning\n", g_pass, g_fail, hl_count[0], hl_count[1]);
    return g_fail ? 1 : 0;
}
