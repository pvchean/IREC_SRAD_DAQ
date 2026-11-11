/* ptp_probe: raw libusb PTP/MTP client for the IREC DAQ spike (MTP-P-10). Helper for Claude, not a deliverable.
 * Build: gcc -O2 -Wall -o ptp_probe ptp_probe.c -lusb-1.0
 * Run:   ./ptp_probe [-c /dev/ttyACM0] [-s STAGE] [-r]
 *   -c PORT  CDC port: sends INFO after each phase, and LOG at the end or on failure (shows that the firmware is alive).
 *   -s N     last stage to run (default 7):
 *            1 status + OpenSession            2 + GetDeviceInfo, GetStorageIDs, GetStorageInfo
 *            3 + GetObjectHandles (root, NAND), GetObjectInfo     4 + GetObject F00002 (100 KB)
 *            5 + GetObject F00001 (3 MB, speed)                   6 + GetPartialObject at random offsets
 *            7 + SendObjectInfo/DeleteObject (must be refused)
 *   -r       after the run: libusb-less PTP DEVICE_RESET class request (0x66), then OpenSession again.
 * Rules (Opus hint): never libusb_reset_device, never set_configuration, never clear_halt. Every transfer has a timeout.
 * Exit code 0 = all checks passed. The data pattern of the fake VFS: byte = (offset & 0xFF) ^ file number. */
#define _GNU_SOURCE
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

#define VID 0x303a
#define PID 0x4041
#define IFACE 2
#define EP_OUT 0x04
#define EP_IN  0x84
#define T_BULK 3000   /* ms, per bulk transfer */
#define CHUNK  16384

enum { C_CMD = 1, C_DATA = 2, C_RSP = 3, C_EVT = 4 };
enum { OP_GETDEVINFO = 0x1001, OP_OPEN = 0x1002, OP_CLOSE = 0x1003, OP_STORIDS = 0x1004, OP_STORINFO = 0x1005,
       OP_HANDLES = 0x1007, OP_OBJINFO = 0x1008, OP_GETOBJ = 0x1009, OP_DELETE = 0x100B, OP_SENDINFO = 0x100C,
       OP_SENDOBJ = 0x100D, OP_PARTIAL = 0x101B };

static libusb_device_handle *h;
static const char *cdc_port;
static uint32_t tid;
static int fails;
static struct timespec t0;

static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6;
}
#define LOG(...) do { printf("[%8.1f] ", now_ms()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) do { fails++; printf("[%8.1f] FAIL: ", now_ms()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void hexdump(const uint8_t *p, size_t n)
{
    printf("           ");
    for (size_t i = 0; i < n && i < 48; i++) { printf("%02x ", p[i]); }
    if (n > 48) { printf("..."); }
    printf("\n");
}

/* ---- CDC side channel: shows whether the firmware task is alive ---- */
static int cdc_fd = -1;
static void cdc_open(void)
{
    if (!cdc_port) { return; }
    cdc_fd = open(cdc_port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (cdc_fd < 0) { LOG("CDC: cannot open %s", cdc_port); return; }
    struct termios t; tcgetattr(cdc_fd, &t); cfmakeraw(&t); tcsetattr(cdc_fd, TCSANOW, &t);
}
static void cdc_cmd(const char *cmd, int wait_ms, int dump_all)
{
    if (cdc_fd < 0) { return; }
    char line[64]; snprintf(line, sizeof line, "%s\r\n", cmd);
    if (write(cdc_fd, line, strlen(line)) < 0) { LOG("CDC: write error"); return; }
    char buf[512]; int got = 0; double end = now_ms() + wait_ms;
    printf("           CDC %s: ", cmd);
    while (now_ms() < end) {
        struct pollfd p = { cdc_fd, POLLIN, 0 };
        if (poll(&p, 1, 50) > 0) {
            ssize_t n = read(cdc_fd, buf, sizeof buf - 1);
            if (n > 0) {
                got += n; buf[n] = 0;
                if (dump_all) { fputs(buf, stdout); } else { for (ssize_t i = 0; i < n; i++) { if (buf[i] != '\r') { putchar(buf[i]); } } }
                if (dump_all && strstr(buf, "END")) { break; }
            } else if (n < 0) { break; }
        }
    }
    if (!got) { printf("(no answer within %d ms: firmware task NOT alive?)", wait_ms); }
    printf("\n"); fflush(stdout);
}
static void cdc_alive(const char *where) { printf("           after %s\n", where); cdc_cmd("INFO", 700, 0); }

/* ---- PTP transactions ---- */
typedef struct { uint16_t rc; uint32_t np; uint32_t p[5]; uint8_t *data; size_t dlen; int err; } ptp_t;

static int bulk(unsigned char ep, uint8_t *buf, int len, int *n, unsigned to)
{
    int r = libusb_bulk_transfer(h, ep, buf, len, n, to);
    return r;
}

static int send_container(uint16_t type, uint16_t code, uint32_t t, const uint32_t *par, int np, const uint8_t *payload, size_t plen)
{
    size_t total = 12 + np * 4 + plen;
    uint8_t *b = calloc(1, total);
    le32(b, total); le16(b + 4, type); le16(b + 6, code); le32(b + 8, t);
    for (int i = 0; i < np; i++) { le32(b + 12 + 4 * i, par[i]); }
    if (plen) { memcpy(b + 12 + np * 4, payload, plen); }
    int sent = 0, r = 0; size_t off = 0;
    while (off < total) {
        int chunk = (int)((total - off) > CHUNK ? CHUNK : (total - off));
        r = bulk(EP_OUT, b + off, chunk, &sent, T_BULK);
        if (r) { break; }
        off += sent;
    }
    /* a container that is a multiple of 64 B needs a ZLP */
    if (!r && total % 64 == 0) { r = bulk(EP_OUT, b, 0, &sent, T_BULK); }
    free(b);
    if (r) { LOG("  OUT error: %s (sent %zu of %zu)", libusb_error_name(r), off, total); }
    return r;
}

/* read one response container (skips ZLPs). Returns 0 on success. */
static int read_response(ptp_t *o, uint32_t want_tid)
{
    uint8_t b[512]; int n = 0, r;
    for (int tries = 0; tries < 4; tries++) {
        r = bulk(EP_IN, b, sizeof b, &n, T_BULK);
        if (r) { LOG("  IN (response) error: %s", libusb_error_name(r)); o->err = r; return r; }
        if (n > 0) { break; }
    }
    if (n < 12) { LOG("  short response (%d B)", n); hexdump(b, n); o->err = -100; return -100; }
    if (rd16(b + 4) != C_RSP) { LOG("  expected a response container, got type %u code %04x len %u", rd16(b + 4), rd16(b + 6), rd32(b)); hexdump(b, n); o->err = -101; return -101; }
    o->rc = rd16(b + 6);
    if (rd32(b + 8) != want_tid) { LOG("  note: response tid %u, expected %u", rd32(b + 8), want_tid); }
    o->np = (n - 12) / 4 > 5 ? 5 : (n - 12) / 4;
    for (uint32_t i = 0; i < o->np; i++) { o->p[i] = rd32(b + 12 + 4 * i); }
    return 0;
}

/* One transaction with no outgoing data. A data phase from the device is collected in o->data. */
static void ptp(ptp_t *o, uint16_t op, const uint32_t *par, int np, const char *name)
{
    memset(o, 0, sizeof *o);
    uint32_t t = tid++;
    double a = now_ms();
    LOG("%s: command (tid %u)", name, t);
    if (send_container(C_CMD, op, t, par, np, NULL, 0)) { o->err = -1; return; }
    uint8_t *buf = malloc(CHUNK); int n = 0, r;
    r = bulk(EP_IN, buf, CHUNK, &n, T_BULK);
    while (!r && n == 0) { r = bulk(EP_IN, buf, CHUNK, &n, T_BULK); }
    if (r) { LOG("  IN (first) error: %s", libusb_error_name(r)); o->err = r; free(buf); return; }
    if (n >= 12 && rd16(buf + 4) == C_DATA) {
        size_t total = rd32(buf);                       /* header + payload */
        o->data = malloc(total > 12 ? total - 12 + 16 : 16);
        size_t got = n - 12;
        memcpy(o->data, buf + 12, got);
        while (12 + got < total) {
            size_t rem = total - 12 - got;
            int want = rem > CHUNK ? CHUNK : (int)((rem + 63) & ~63u);
            r = bulk(EP_IN, buf, want, &n, T_BULK);
            if (r) { LOG("  IN (data) error: %s after %zu of %zu B", libusb_error_name(r), got, total - 12); o->err = r; free(buf); o->dlen = got; return; }
            if (n == 0) { if (12 + got >= total) { break; } continue; }
            if (got + n > total - 12) { n = total - 12 - got; }
            memcpy(o->data + got, buf, n); got += n;
        }
        o->dlen = got;
        free(buf);
        if (read_response(o, t)) { return; }
    } else {
        /* no data phase: this is the response */
        if (n >= 12 && rd16(buf + 4) == C_RSP) {
            o->rc = rd16(buf + 6);
            o->np = (n - 12) / 4 > 5 ? 5 : (n - 12) / 4;
            for (uint32_t i = 0; i < o->np; i++) { o->p[i] = rd32(buf + 12 + 4 * i); }
        } else { LOG("  unexpected first packet (%d B)", n); hexdump(buf, n); o->err = -102; }
        free(buf);
    }
    LOG("%s: response 0x%04x, data %zu B, %.1f ms", name, o->rc, o->dlen, now_ms() - a);
}

static void ptp_free(ptp_t *o) { free(o->data); o->data = NULL; }

static int utf16_name(const uint8_t *p, size_t avail, char *out, size_t outsz)
{
    if (avail < 1) { out[0] = 0; return 0; }
    int nch = p[0]; size_t k = 0;
    for (int i = 0; i < nch && (size_t)(1 + 2 * i + 1) < avail; i++) {
        uint16_t c = rd16(p + 1 + 2 * i);
        if (c == 0) { break; }
        if (k + 1 < outsz) { out[k++] = c < 128 ? (char)c : '?'; }
    }
    out[k] = 0;
    return 1 + nch * 2;
}

typedef struct { uint32_t h; uint16_t fmt, prot; uint32_t size, parent; char name[64]; } obj_t;

static int get_objinfo(uint32_t handle, obj_t *ob)
{
    ptp_t r; uint32_t par[1] = { handle };
    ptp(&r, OP_OBJINFO, par, 1, "GetObjectInfo");
    int ok = (!r.err && r.rc == 0x2001 && r.dlen >= 53);
    if (ok) {
        ob->h = handle; ob->fmt = rd16(r.data + 4); ob->prot = rd16(r.data + 6); ob->size = rd32(r.data + 8);
        ob->parent = rd32(r.data + 38);
        utf16_name(r.data + 52, r.dlen - 52, ob->name, sizeof ob->name);
        LOG("  handle %u: '%s' format 0x%04x prot %u size %u parent 0x%x", handle, ob->name, ob->fmt, ob->prot, ob->size, ob->parent);
    } else { FAIL("GetObjectInfo(%u) rc 0x%04x err %d", handle, r.rc, r.err); }
    ptp_free(&r);
    return ok;
}

static int list_handles(uint32_t parent, uint32_t *out, int max, const char *what)
{
    ptp_t r; uint32_t par[3] = { 0xFFFFFFFF, 0, parent };
    ptp(&r, OP_HANDLES, par, 3, what);
    int cnt = 0;
    if (!r.err && r.rc == 0x2001 && r.dlen >= 4) {
        cnt = rd32(r.data);
        if (cnt > max) { cnt = max; }
        for (int i = 0; i < cnt && r.dlen >= (size_t)(8 + 4 * i); i++) { out[i] = rd32(r.data + 4 + 4 * i); }
        LOG("  %d handle(s)", cnt);
    } else { FAIL("%s rc 0x%04x err %d", what, r.rc, r.err); cnt = -1; }
    ptp_free(&r);
    return cnt;
}

static int fileno_of(const char *name) { return (name[0] == 'F') ? atoi(name + 1) : 0; }
static int check_pattern(const uint8_t *d, size_t n, uint64_t off, int fno)
{
    for (size_t i = 0; i < n; i++) {
        if (d[i] != (uint8_t)(((off + i) & 0xFF) ^ fno)) { LOG("  PATTERN MISMATCH at offset %llu: got %02x want %02x", (unsigned long long)(off + i), d[i], (uint8_t)(((off + i) & 0xFF) ^ fno)); return 0; }
    }
    return 1;
}

static void get_object(const obj_t *ob)
{
    ptp_t r; uint32_t par[1] = { ob->h };
    double a = now_ms();
    ptp(&r, OP_GETOBJ, par, 1, "GetObject");
    double dt = now_ms() - a;
    if (r.err || r.rc != 0x2001) { FAIL("GetObject '%s' rc 0x%04x err %d got %zu B", ob->name, r.rc, r.err, r.dlen); }
    else if (r.dlen != ob->size) { FAIL("GetObject '%s': %zu B, expected %u", ob->name, r.dlen, ob->size); }
    else if (!check_pattern(r.data, r.dlen, 0, fileno_of(ob->name))) { FAIL("GetObject '%s': pattern wrong", ob->name); }
    else { LOG("GetObject '%s' OK: %zu B in %.0f ms = %.1f KB/s", ob->name, r.dlen, dt, r.dlen / 1024.0 / (dt / 1000.0)); }
    ptp_free(&r);
}

static void partial_tests(const obj_t *ob)
{
    srand(12345);
    int fno = fileno_of(ob->name);
    for (int i = 0; i < 20; i++) {
        uint32_t off, len;
        if (i == 0) { off = 0; len = 1; }
        else if (i == 1) { off = ob->size - 100; len = 100; }
        else if (i == 2) { off = 63; len = 130; }
        else { off = (uint32_t)rand() % (ob->size - 1); len = 1 + (uint32_t)rand() % (i % 3 == 0 ? 60000 : 5000); if (off + len > ob->size) { len = ob->size - off; } }
        ptp_t r; uint32_t par[3] = { ob->h, off, len };
        double a = now_ms();
        ptp(&r, OP_PARTIAL, par, 3, "GetPartialObject");
        double dt = now_ms() - a;
        if (r.err || r.rc != 0x2001) { FAIL("partial off %u len %u: rc 0x%04x err %d", off, len, r.rc, r.err); ptp_free(&r); break; }
        if (r.np < 1 || r.p[0] != len || r.dlen != len) { FAIL("partial off %u len %u: got %zu B, resp param %u", off, len, r.dlen, r.np ? r.p[0] : 0); }
        else if (!check_pattern(r.data, r.dlen, off, fno)) { FAIL("partial off %u len %u: pattern wrong", off, len); }
        else { LOG("  partial off %u len %u OK (%.1f ms)", off, len, dt); }
        ptp_free(&r);
    }
    /* beyond EOF: must give an error or a short read, never hang */
    ptp_t r; uint32_t par[3] = { ob->h, ob->size + 10, 100 };
    ptp(&r, OP_PARTIAL, par, 3, "GetPartialObject(beyond EOF)");
    LOG("  beyond EOF: rc 0x%04x err %d data %zu B (an error code is the expected answer)", r.rc, r.err, r.dlen);
    ptp_free(&r);
}

static void write_refusal(void)
{
    /* SendObjectInfo with a small ObjectInfo dataset: expect a refusal (0x200E StoreReadOnly / 0x200F AccessDenied). */
    uint8_t d[128]; memset(d, 0, sizeof d); size_t k = 0;
    le32(d + k, 0x00010001); k += 4;      /* storage id (what libmtp saw; refusal is expected anyway) */
    le16(d + k, 0x3000); k += 2;          /* format: undefined */
    le16(d + k, 0); k += 2;
    le32(d + k, 16); k += 4;              /* size */
    k += 2 + 4 * 6;                       /* thumb fields, image fields */
    le32(d + k, 0); k += 4;               /* parent */
    le16(d + k, 0); k += 2;
    le32(d + k, 0); k += 4;
    le32(d + k, 0); k += 4;
    const char *nm = "W.TXT"; d[k++] = strlen(nm) + 1;
    for (const char *c = nm; *c; c++) { le16(d + k, *c); k += 2; }
    le16(d + k, 0); k += 2;
    d[k++] = 0; d[k++] = 0; d[k++] = 0;   /* empty date strings and keywords */

    uint32_t t = tid++;
    uint32_t par[2] = { 0xFFFFFFFF, 0xFFFFFFFF };
    LOG("SendObjectInfo (write test, expect a refusal)");
    if (send_container(C_CMD, OP_SENDINFO, t, par, 2, NULL, 0)) { FAIL("SendObjectInfo command failed to send"); return; }
    uint8_t b[512]; int n = 0;
    int r = bulk(EP_IN, b, sizeof b, &n, 1000);          /* a refusal comes right away */
    if (!r && n >= 12 && rd16(b + 4) == C_RSP) { LOG("  refused at command phase: rc 0x%04x (%s)", rd16(b + 6), rd16(b + 6) == 0x200E || rd16(b + 6) == 0x200F ? "OK, as designed" : "unexpected code"); if (rd16(b + 6) == 0x2001) { FAIL("write accepted!"); } }
    else {
        LOG("  no immediate response (%s), sending the data phase", r ? libusb_error_name(r) : "other");
        if (send_container(C_DATA, OP_SENDINFO, t, NULL, 0, d, k)) { FAIL("data phase failed to send"); return; }
        ptp_t o; if (!read_response(&o, t)) { LOG("  response 0x%04x (expected 0x200E or 0x200F)", o.rc); if (o.rc == 0x2001) { FAIL("write accepted!"); } } else { FAIL("no response to SendObjectInfo"); }
    }
    ptp_t o; uint32_t dp[2] = { 0x00000001, 0 };
    ptp(&o, OP_DELETE, dp, 2, "DeleteObject(1) (expect refusal)");
    if (o.err) { FAIL("DeleteObject: no clean response"); }
    else if (o.rc == 0x2001) { FAIL("delete accepted!"); }
    else { LOG("  delete refused with 0x%04x: OK", o.rc); }
    ptp_free(&o);
}

int main(int argc, char **argv)
{
    int stage = 7, do_reset = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { cdc_port = argv[++i]; }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) { stage = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-r")) { do_reset = 1; }
        else { fprintf(stderr, "usage: %s [-c cdcport] [-s stage 1..7] [-r]\n", argv[0]); return 2; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cdc_open();
    cdc_alive("start");
    if (libusb_init(NULL)) { fprintf(stderr, "libusb_init failed\n"); return 2; }
    h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!h) { fprintf(stderr, "device %04x:%04x not found or no access\n", VID, PID); return 2; }
    int r = libusb_claim_interface(h, IFACE);
    LOG("claim interface %d: %s", IFACE, libusb_error_name(r));
    if (r) { return 2; }

    /* A: class request GET_DEVICE_STATUS */
    uint8_t st[32]; memset(st, 0, sizeof st);
    r = libusb_control_transfer(h, 0xA1, 0x67, 0, IFACE, st, 20, 1000);
    LOG("GET_DEVICE_STATUS: %s (%d B)", r < 0 ? libusb_error_name(r) : "ok", r);
    if (r > 0) { hexdump(st, r); }
    cdc_alive("GET_DEVICE_STATUS");

    /* B: OpenSession */
    ptp_t o; uint32_t one[1] = { 1 };
    tid = 0;
    ptp(&o, OP_OPEN, one, 1, "OpenSession");
    if (o.err || (o.rc != 0x2001 && o.rc != 0x201E)) { FAIL("OpenSession failed: rc 0x%04x err %d", o.rc, o.err); cdc_alive("OpenSession (FAILED)"); cdc_cmd("LOG", 3000, 1); goto out; }
    if (o.rc == 0x201E) { LOG("  session was already open (0x201E): ok"); }
    ptp_free(&o);
    cdc_alive("OpenSession");
    if (stage < 2) { goto close; }

    ptp(&o, OP_GETDEVINFO, NULL, 0, "GetDeviceInfo");
    if (o.err || o.rc != 0x2001) { FAIL("GetDeviceInfo rc 0x%04x err %d", o.rc, o.err); }
    else {
        size_t k = 8; char s[64];                       /* std version(2) vendor ext id(4) mtp version(2) */
        k += utf16_name(o.data + k, o.dlen - k, s, sizeof s); LOG("  vendor ext: '%s'", s);
        k += 2;                                         /* functional mode */
        uint32_t nops = rd32(o.data + k); LOG("  %u operations supported", nops); k += 4 + nops * 2;
    }
    ptp_free(&o);
    cdc_alive("GetDeviceInfo");

    ptp(&o, OP_STORIDS, NULL, 0, "GetStorageIDs");
    uint32_t sid = 0;
    if (o.err || o.rc != 0x2001 || o.dlen < 8) { FAIL("GetStorageIDs rc 0x%04x err %d", o.rc, o.err); }
    else { LOG("  %u storage(s), first id 0x%08x", rd32(o.data), rd32(o.data + 4)); sid = rd32(o.data + 4); }
    ptp_free(&o);
    if (sid) {
        uint32_t par[1] = { sid };
        ptp(&o, OP_STORINFO, par, 1, "GetStorageInfo");
        if (o.err || o.rc != 0x2001 || o.dlen < 26) { FAIL("GetStorageInfo rc 0x%04x err %d", o.rc, o.err); }
        else {
            char desc[64]; utf16_name(o.data + 26, o.dlen - 26, desc, sizeof desc);
            LOG("  type %u fs %u access %u capacity %llu free %llu desc '%s'", rd16(o.data), rd16(o.data + 2), rd16(o.data + 4),
                (unsigned long long)rd64(o.data + 6), (unsigned long long)rd64(o.data + 14), desc);
        }
        ptp_free(&o);
    }
    cdc_alive("storage calls");
    if (stage < 3) { goto close; }

    uint32_t hs[64]; int n = list_handles(0xFFFFFFFF, hs, 64, "GetObjectHandles(root)");
    uint32_t nand = 0;
    for (int i = 0; i < n; i++) { obj_t ob; if (get_objinfo(hs[i], &ob) && !strcmp(ob.name, "NAND") && ob.fmt == 0x3001) { nand = ob.h; } }
    obj_t f1 = { 0 }, f2 = { 0 };
    if (!nand) { FAIL("no NAND folder in the root"); }
    else {
        n = list_handles(nand, hs, 64, "GetObjectHandles(NAND)");
        for (int i = 0; i < n; i++) {
            obj_t ob;
            if (get_objinfo(hs[i], &ob)) { if (!strcmp(ob.name, "F00001.BIN")) { f1 = ob; } if (!strcmp(ob.name, "F00002.BIN")) { f2 = ob; } }
        }
        if (!f1.h || !f2.h) { FAIL("F00001.BIN or F00002.BIN missing"); }
        else if (f1.size != 3u * 1024 * 1024 || f2.size != 100u * 1024) { FAIL("file sizes wrong: %u %u", f1.size, f2.size); }
    }
    cdc_alive("object listing");
    if (stage < 4 || !f2.h) { goto close; }

    get_object(&f2);
    cdc_alive("GetObject F00002");
    if (stage < 5 || !f1.h) { goto close; }

    get_object(&f1);
    cdc_alive("GetObject F00001 (3 MB)");
    if (stage < 6) { goto close; }

    partial_tests(&f1);
    cdc_alive("GetPartialObject");
    if (stage < 7) { goto close; }

    write_refusal();
    cdc_alive("write refusal");

close:
    ptp(&o, OP_CLOSE, NULL, 0, "CloseSession");
    ptp_free(&o);
    cdc_alive("CloseSession");
    if (do_reset) {
        r = libusb_control_transfer(h, 0x21, 0x66, 0, IFACE, NULL, 0, 1000);
        LOG("DEVICE_RESET class request: %s", r < 0 ? libusb_error_name(r) : "ok (expected: ok within 1 s after patch P1a)");
        cdc_alive("DEVICE_RESET");
        tid = 0; ptp(&o, OP_OPEN, one, 1, "OpenSession (after reset)");
        if (o.err || (o.rc != 0x2001 && o.rc != 0x201E)) { FAIL("OpenSession after reset failed"); }
        ptp_free(&o);
        ptp(&o, OP_CLOSE, NULL, 0, "CloseSession"); ptp_free(&o);
    }
out:
    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(NULL);
    if (cdc_fd >= 0) { if (fails) { cdc_cmd("LOG", 3000, 1); } close(cdc_fd); }
    LOG("%s (%d failure%s)", fails ? "RESULT: FAIL" : "RESULT: PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
