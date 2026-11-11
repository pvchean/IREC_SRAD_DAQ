/* Stubs for the IDF, FreeRTOS, the VFS layer and the TinyUSB device layer (Claude helper, not a deliverable).
   The USB part is a MOCK of the usbd_* layer under the REAL class driver mtp_device.c:
     * a device endpoint transfer is armed with usbd_edpt_xfer(); it completes only when the host moves the data
       (OUT: the host wrote packets, IN: the host read all packets), as on the real controller;
     * the completion is an event; um_pump() runs the events in order, like the TinyUSB device task;
     * bulk packet size 64 (full speed); a transfer ends at the full length or at a short packet or a ZLP;
     * endpoint buffers are read and written live (no copy), so a buffer that is reused too early shows as bad data;
     * usbd_edpt_stall() sets stalled and busy; usbd_edpt_clear_stall() clears both (TinyUSB behaviour). */
#include <pthread.h>
#include <stdarg.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_descriptors.h"

#include "tusb.h"
#include "device/usbd_pvt.h"
#include "hosttest.h"

/* ------------------------------------------------------------------ logging */
int hl_verbose;
int hl_count[5];
static char s_last_err[600];
const char *hl_last_error(void) { return s_last_err; }

void host_log(int level, const char *tag, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    hl_count[level]++;
    if (level == 0) { snprintf(s_last_err, sizeof s_last_err, "%s: %s", tag, msg); }
    if (hl_verbose >= 1 && (level <= 1 || hl_verbose >= 2)) {
        static const char L[] = "EWIDV";
        fprintf(stderr, "    [%c] %s: %s\n", L[level], tag, msg);
    }
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total, uint64_t *free_bytes)
{
    (void)base_path; (void)total; (void)free_bytes;
    return ESP_FAIL;                         /* the daq storage has total_bytes set: this must never be asked */
}

const char *tinyusb_desc_serial_number_get(void) { return "AABBCCDDEEFF"; }

int h_wdt_resets;
esp_err_t esp_task_wdt_reset(void) { h_wdt_resets++; return ESP_OK; }

/* ------------------------------------------------------------------ FreeRTOS on pthreads */
struct host_sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    int count, is_mutex, held;
    pthread_t owner;
};

static SemaphoreHandle_t sem_new(int is_mutex, int count)
{
    struct host_sem *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->is_mutex = is_mutex;
    s->count = count;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return sem_new(1, 1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return sem_new(0, 0); }
void vSemaphoreDelete(SemaphoreHandle_t s) { if (s) { pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->c); free(s); } }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    pthread_mutex_lock(&s->m);
    if (s->is_mutex && s->held && pthread_equal(s->owner, pthread_self())) {
        fprintf(stderr, "HOSTTEST FATAL: a task took a non-recursive mutex twice (it would deadlock on the board)\n");
        abort();
    }
    struct timespec dl;
    if (ticks != portMAX_DELAY) {
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += ticks / 1000;
        dl.tv_nsec += (long)(ticks % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    while (s->count == 0) {
        if (ticks == portMAX_DELAY) { pthread_cond_wait(&s->c, &s->m); }
        else if (pthread_cond_timedwait(&s->c, &s->m, &dl) != 0 && s->count == 0) { pthread_mutex_unlock(&s->m); return pdFALSE; }
    }
    s->count--;
    if (s->is_mutex) { s->held = 1; s->owner = pthread_self(); }
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    if (s->is_mutex) { s->held = 0; }
    if (s->count < 1) { s->count++; }
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

struct host_task { pthread_t th; pthread_mutex_t m; pthread_cond_t c; uint32_t notify; TaskFunction_t fn; void *arg; };
static __thread struct host_task *t_self;

static void *task_tramp(void *a)
{
    struct host_task *t = a;
    t_self = t;
    t->fn(t->arg);
    return NULL;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, unsigned prio, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio;
    struct host_task *t = calloc(1, sizeof *t);
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    t->fn = fn;
    t->arg = arg;
    if (out) { *out = t; }
    pthread_create(&t->th, NULL, task_tramp, t);
    pthread_detach(t->th);
    return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    (void)ticks;
    struct host_task *t = t_self;
    pthread_mutex_lock(&t->m);
    while (t->notify == 0) { pthread_cond_wait(&t->c, &t->m); }
    uint32_t v = t->notify;
    t->notify = clear ? 0 : v - 1;
    pthread_mutex_unlock(&t->m);
    return v;
}
BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    pthread_mutex_lock(&t->m);
    t->notify++;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return pdPASS;
}

/* ------------------------------------------------------------------ VFS table and the POSIX redirect */
static esp_vfs_t s_vfs;
static void *s_vfs_ctx;
static bool s_vfs_ok;
int h_vfs_write_calls, h_vfs_opens, h_vfs_reads;

esp_err_t esp_vfs_register(const char *base_path, const esp_vfs_t *vfs, void *ctx)
{
    if (strcmp(base_path, "/daq") != 0) { return ESP_FAIL; }
    s_vfs = *vfs;
    s_vfs_ctx = ctx;
    s_vfs_ok = true;
    return ESP_OK;
}

/* returns true and the path below the mount point (IDF strips the base path) */
static bool route(const char *p, const char **rest)
{
    if (!s_vfs_ok || strncmp(p, "/daq", 4) != 0 || (p[4] != 0 && p[4] != '/')) { return false; }
    *rest = p + 4;
    return true;
}

typedef struct { uint32_t magic; int fd; bool eof, err; } hfile_t;
#define HFILE_MAGIC 0x48464C45U

FILE *host_fopen(const char *p, const char *m)
{
    const char *rest;
    if (!route(p, &rest)) { errno = ENOENT; return NULL; }
    int flags = O_RDONLY;
    if (strchr(m, 'w')) { flags = O_WRONLY | O_CREAT | O_TRUNC; }
    else if (strchr(m, 'a')) { flags = O_WRONLY | O_CREAT | O_APPEND; }
    else if (strchr(m, '+')) { flags = O_RDWR; }
    if (flags != O_RDONLY) { h_vfs_write_calls++; }
    h_vfs_opens++;
    int fd = s_vfs.open_p(s_vfs_ctx, rest, flags, 0666);
    if (fd < 0) { return NULL; }
    hfile_t *f = calloc(1, sizeof *f);
    f->magic = HFILE_MAGIC;
    f->fd = fd;
    return (FILE *)f;
}

size_t host_fread(void *b, size_t s, size_t n, FILE *fp)
{
    hfile_t *f = (hfile_t *)fp;
    if (f->magic != HFILE_MAGIC || s == 0 || n == 0) { return 0; }
    size_t want = s * n, got = 0;
    h_vfs_reads++;
    while (got < want) {
        ssize_t r = s_vfs.read_p(s_vfs_ctx, f->fd, (uint8_t *)b + got, want - got);
        if (r < 0) { f->err = true; break; }
        if (r == 0) { f->eof = true; break; }
        got += (size_t)r;
    }
    return got / s;
}

size_t host_fwrite(const void *b, size_t s, size_t n, FILE *fp)
{
    hfile_t *f = (hfile_t *)fp;
    h_vfs_write_calls++;
    (void)s_vfs.write_p(s_vfs_ctx, f->fd, b, s * n);
    return 0;
}

int host_fseek(FILE *fp, long off, int wh)
{
    hfile_t *f = (hfile_t *)fp;
    f->eof = false;
    return s_vfs.lseek_p(s_vfs_ctx, f->fd, off, wh) < 0 ? -1 : 0;
}
int host_fclose(FILE *fp) { hfile_t *f = (hfile_t *)fp; int rc = s_vfs.close_p(s_vfs_ctx, f->fd); free(f); return rc; }
int host_fflush(FILE *fp) { (void)fp; return 0; }

int host_stat(const char *p, struct stat *st)
{
    const char *rest;
    if (!route(p, &rest)) { return stat(p, st); }     /* not redirected here (this unit has no macro) */
    return s_vfs.stat_p(s_vfs_ctx, rest, st);
}
DIR *host_opendir(const char *p)
{
    const char *rest;
    if (!route(p, &rest)) { errno = ENOENT; return NULL; }
    return s_vfs.opendir_p(s_vfs_ctx, rest);
}
struct dirent *host_readdir(DIR *d) { return s_vfs.readdir_p(s_vfs_ctx, d); }
int host_closedir(DIR *d) { return s_vfs.closedir_p(s_vfs_ctx, d); }
int host_unlink(const char *p) { const char *r; h_vfs_write_calls++; return route(p, &r) ? s_vfs.unlink_p(s_vfs_ctx, r) : (errno = ENOENT, -1); }
int host_rename(const char *a, const char *b) { const char *r, *r2; h_vfs_write_calls++; return (route(a, &r) && route(b, &r2)) ? s_vfs.rename_p(s_vfs_ctx, r, r2) : (errno = ENOENT, -1); }
int host_rmdir(const char *p) { const char *r; h_vfs_write_calls++; return route(p, &r) ? s_vfs.rmdir_p(s_vfs_ctx, r) : (errno = ENOENT, -1); }
int host_mkdir(const char *p, mode_t m) { const char *r; h_vfs_write_calls++; return route(p, &r) ? s_vfs.mkdir_p(s_vfs_ctx, r, m) : (errno = ENOENT, -1); }
int host_truncate(const char *p, off_t l) { (void)p; (void)l; h_vfs_write_calls++; errno = ENOSYS; return -1; }

/* ------------------------------------------------------------------ USB device layer mock */
typedef struct { uint8_t d[PKT]; uint8_t n; } pkt_t;
#define QCAP 16384
static pkt_t *s_outq;
static size_t s_oq_h, s_oq_t;                 /* ring, head = next to read */

typedef struct {
    bool busy, claimed, stalled, armed;
    uint8_t *buf;
    uint16_t len, done;                      /* OUT: done = bytes received; IN: done = bytes the host read */
} ep_t;
static ep_t s_ep[3];                         /* 0 = OUT, 1 = IN, 2 = EVT */
typedef struct { uint8_t ep; uint16_t n; } ev_t;
static ev_t s_ev[64];
static size_t s_ev_h, s_ev_t;

static int epi(uint8_t a) { return a == EP_OUT ? 0 : a == EP_IN ? 1 : 2; }
static void ev_push(uint8_t ep, uint16_t n) { s_ev[s_ev_t++ % 64] = (ev_t){ ep, n }; }

static struct { uint8_t *buf; uint16_t len; bool status; bool used; } s_ctl;

bool usbd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *d) { (void)rhport; (void)d; return true; }

bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const *p, uint8_t cnt, uint8_t type, uint8_t *ep_out, uint8_t *ep_in)
{
    (void)rhport;
    for (int i = 0; i < cnt; i++) {
        const tusb_desc_endpoint_t *d = (const tusb_desc_endpoint_t *)p;
        if (d->bDescriptorType != TUSB_DESC_ENDPOINT || d->bmAttributes.xfer != type) { return false; }
        if (d->bEndpointAddress & 0x80) { *ep_in = d->bEndpointAddress; } else { *ep_out = d->bEndpointAddress; }
        p += d->bLength;
    }
    return true;
}

bool usbd_edpt_busy(uint8_t rhport, uint8_t ep) { (void)rhport; return s_ep[epi(ep)].busy; }

bool usbd_edpt_claim(uint8_t rhport, uint8_t ep)
{
    (void)rhport;
    ep_t *e = &s_ep[epi(ep)];
    if (e->busy || e->claimed) { return false; }
    e->claimed = true;
    return true;
}
bool usbd_edpt_release(uint8_t rhport, uint8_t ep) { (void)rhport; s_ep[epi(ep)].claimed = false; return true; }

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *buf, uint16_t len, bool is_isr)
{
    (void)rhport; (void)is_isr;
    ep_t *e = &s_ep[epi(ep)];
    if (e->busy && !e->claimed) { return false; }
    if (e->stalled) { return false; }
    e->claimed = false;
    e->busy = true;
    e->armed = true;
    e->buf = buf;
    e->len = len;
    e->done = 0;
    return true;
}

void usbd_edpt_stall(uint8_t rhport, uint8_t ep)
{
    (void)rhport;
    ep_t *e = &s_ep[epi(ep)];
    e->stalled = true;
    e->busy = true;
    e->armed = false;
}
void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep)
{
    (void)rhport;
    ep_t *e = &s_ep[epi(ep)];
    e->stalled = false;
    e->busy = false;
    e->claimed = false;
    e->armed = false;
}
bool usbd_edpt_stalled(uint8_t rhport, uint8_t ep) { (void)rhport; return s_ep[epi(ep)].stalled; }

tusb_speed_t tud_speed_get(void) { return TUSB_SPEED_FULL; }

bool tud_control_xfer(uint8_t rhport, tusb_control_request_t const *r, void *buf, uint16_t len)
{
    (void)rhport; (void)r;
    s_ctl.buf = buf; s_ctl.len = len; s_ctl.used = true;
    return true;
}
bool tud_control_status(uint8_t rhport, tusb_control_request_t const *r) { (void)rhport; (void)r; s_ctl.status = true; return true; }

/* ---- host side of the mock ---- */
bool um_stalled(uint8_t ep) { return s_ep[epi(ep)].stalled; }
void um_clear_halt(uint8_t ep) { usbd_edpt_clear_stall(0, ep); }
size_t um_out_pending(void) { return (s_oq_t - s_oq_h); }
bool um_in_pending(void) { return s_ep[1].armed; }

static const uint8_t s_desc[] = {
    9, TUSB_DESC_INTERFACE, 2, 0, 3, TUSB_CLASS_IMAGE, 1, 1, 0,
    7, TUSB_DESC_ENDPOINT, EP_EVT, TUSB_XFER_INTERRUPT, 28, 0, 6,
    7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, 64, 0, 0,
};

void um_reset(void)
{
    if (!s_outq) { s_outq = calloc(QCAP, sizeof(pkt_t)); }
    s_oq_h = s_oq_t = 0;
    s_ev_h = s_ev_t = 0;
    memset(s_ep, 0, sizeof s_ep);
    memset(&s_ctl, 0, sizeof s_ctl);
    mtpd_init();
    uint16_t used = mtpd_open(0, (const tusb_desc_interface_t *)s_desc, sizeof s_desc);
    if (used == 0) { fprintf(stderr, "HOSTTEST FATAL: mtpd_open failed\n"); abort(); }
}

int um_pump(void)
{
    int handled = 0;
    for (;;) {
        bool progress = false;
        ep_t *o = &s_ep[0];
        while (o->armed && !o->stalled && s_oq_h != s_oq_t) {
            pkt_t *p = &s_outq[s_oq_h % QCAP];
            s_oq_h++;
            memcpy(o->buf + o->done, p->d, p->n);
            o->done = (uint16_t)(o->done + p->n);
            progress = true;
            if (p->n < PKT || o->done >= o->len) {
                o->armed = false;
                ev_push(EP_OUT, o->done);
                break;
            }
        }
        if (s_ev_h != s_ev_t) {
            ev_t ev = s_ev[s_ev_h++ % 64];
            ep_t *e = &s_ep[epi(ev.ep)];
            e->busy = false;
            e->claimed = false;
            mtpd_xfer_cb(0, ev.ep, XFER_RESULT_SUCCESS, ev.n);
            handled++;
            progress = true;
        }
        if (!progress) { break; }
    }
    return handled;
}

int um_host_out(const uint8_t *d, size_t n)
{
    if (s_ep[0].stalled) { return -2; }
    for (size_t off = 0; off < n; off += PKT) {
        pkt_t *p = &s_outq[s_oq_t++ % QCAP];
        p->n = (uint8_t)(n - off > PKT ? PKT : n - off);
        memcpy(p->d, d + off, p->n);
    }
    if (n % PKT == 0) {                                  /* a transfer that ends on a packet boundary ends with a ZLP */
        pkt_t *p = &s_outq[s_oq_t++ % QCAP];
        p->n = 0;
    }
    for (int spin = 0; spin < 8; spin++) {               /* blocking write: wait until the device has read everything */
        um_pump();
        if (s_oq_h == s_oq_t) { return 0; }
        if (s_ep[0].stalled) { s_oq_h = s_oq_t; return -2; }
    }
    s_oq_h = s_oq_t;                                     /* timeout: the host drops the transfer */
    return -1;
}

int um_host_in_packet(uint8_t *p, size_t *n)
{
    ep_t *e = &s_ep[1];
    for (int spin = 0; spin < 4 && !e->armed && !e->stalled; spin++) { um_pump(); }
    if (e->stalled) { return -2; }
    if (!e->armed) { return -1; }
    size_t left = e->len - e->done;
    size_t k = left > PKT ? PKT : left;                  /* len 0 = one ZLP */
    if (k) { memcpy(p, e->buf + e->done, k); }
    e->done = (uint16_t)(e->done + k);
    *n = k;
    if (e->done >= e->len) { e->armed = false; ev_push(EP_IN, e->len); }
    return 0;
}

int um_control(int dir_in, uint8_t req, uint16_t wlen, const uint8_t *out, uint8_t *in, int *in_len)
{
    tusb_control_request_t r;
    memset(&r, 0, sizeof r);
    r.bmRequestType_bit.recipient = TUSB_REQ_RCPT_INTERFACE;
    r.bmRequestType_bit.type = TUSB_REQ_TYPE_CLASS;
    r.bmRequestType_bit.direction = dir_in ? TUSB_DIR_IN : TUSB_DIR_OUT;
    r.bRequest = req;
    r.wIndex = 2;
    r.wLength = wlen;
    memset(&s_ctl, 0, sizeof s_ctl);
    if (!mtpd_control_xfer_cb(0, CONTROL_STAGE_SETUP, &r)) { return -1; }
    if (dir_in) {
        int k = 0;
        if (s_ctl.used && s_ctl.buf) { k = s_ctl.len < wlen ? s_ctl.len : wlen; if (in) { memcpy(in, s_ctl.buf, (size_t)k); } }
        if (in_len) { *in_len = k; }
    } else if (s_ctl.used && s_ctl.buf && wlen) {
        memcpy(s_ctl.buf, out, wlen < s_ctl.len ? wlen : s_ctl.len);
        if (!mtpd_control_xfer_cb(0, CONTROL_STAGE_DATA, &r)) { return -1; }
    }
    if (!mtpd_control_xfer_cb(0, CONTROL_STAGE_ACK, &r)) { return -2; }
    um_pump();
    return 0;
}
