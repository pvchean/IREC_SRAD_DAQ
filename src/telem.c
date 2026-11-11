#include "telem.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "cal.h"
#include "io.h"
#include "stream.h"
#define TP_CRC32(p, n) esp_rom_crc32_le(0, (p), (n))
#include "telem_frame.h"

#define NVS_NS        "daq"
#define NVS_KEY       "telem"
#define CFG_MAGIC     0x7E1E0001U
#define RING_RECS     16384U                /* power of 2: 144 KB in the PSRAM, about 1 minute at 240 records per second (two DAQs) */
#define NDEV          8U                    /* device IDs 0..7 (ADDR2..ADDR0) */
#define BATCH_RECS    250U                  /* records per frame: 2000 bytes, below TP_MAX_UPLOAD */
#define SEND_MS       500                   /* the upload DAQ sends the data of the last 500 ms in one frame per sensor */
#define STATUS_MS     2000
#define CAL_MS        30000
#define GET_BYTES     4096U                 /* max_bytes of a GET */
#define GET_WAIT_MS   5000
#define DL_BUF        6144U
#define RESUME_MAX_US (10LL * 1000000LL)    /* after a longer outage the DOWNLOAD DAQ skips the missed data (it stays in the server archive) */

typedef struct {
    uint32_t magic;
    uint8_t  mode;
    uint8_t  rsv;
    uint16_t port;
    uint16_t decim_pt, decim_lc;
    char     ssid[33], pass[65], host[64], token[17];
} cfg_t;

static const char *TAG = "telem";

volatile uint8_t telem_run_mode;

static cfg_t s_cfg;                         /* the saved settings (the commands change them) */
static cfg_t s_run;                         /* the settings of this boot */
/* the ring of the UPLOAD DAQ. The writers are the sink (own sensors, core 1) and the two chain receive tasks (other boards, core 0),
   so the write, the decimation counters and the calibration blocks are all under s_rmux. The task reads. A full ring drops the oldest record. */
typedef struct __attribute__((packed)) { record_t r; uint8_t dev; } ring_rec_t;
static ring_rec_t *s_ring;
static uint32_t s_rhead, s_rtail;
static uint16_t s_cnt[NDEV][2];             /* decimation counters of each device: [0] PT, [1] load cell */
static portMUX_TYPE s_rmux = portMUX_INITIALIZER_UNLOCKED;
static bool s_local_data;                   /* the own sensors gave a sample: this board is a data source too */

/* The last calibration block that the chain gave for each other board. ver counts the blocks that came. */
typedef struct { record_t b[CAL_BLOCK_RECS]; uint32_t ver; bool valid; } rcal_t;
static rcal_t s_rcal[NDEV];

typedef struct {
    record_t pt[NDEV][BATCH_RECS], lc[NDEV][BATCH_RECS];     /* the batch of each device and sensor */
    unsigned npt[NDEV], nlc[NDEV];
    uint8_t out[TP_HDR + BATCH_RECS * TP_REC_SIZE + TP_CRC + 64U];
} up_t;
static up_t *s_up;                           /* in the PSRAM, only in UPLOAD mode: OFF mode keeps no RAM for it */

static volatile uint32_t s_drops, s_sent_frames, s_reconnects;
static volatile bool s_wifi_up, s_link_up;
static volatile int s_rssi;
static bool s_had_link;

/* ---------------------------------------------------------------- settings */

static bool cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) { return false; }
    esp_err_t e = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof s_cfg);
    if (e == ESP_OK) { e = nvs_commit(h); }
    nvs_close(h);
    return e == ESP_OK;
}

static void cfg_defaults(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = CFG_MAGIC;
    c->mode = TELEM_OFF;
    c->port = TELEM_DEFAULT_PORT;
    c->decim_pt = TELEM_DEFAULT_DECIM_PT;
    c->decim_lc = TELEM_DEFAULT_DECIM_LC;
}

void telem_init(void)
{
    cfg_defaults(&s_cfg);
    nvs_handle_t h;
    cfg_t c;
    size_t len = sizeof c;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &c, &len) == ESP_OK && len == sizeof c && c.magic == CFG_MAGIC && c.mode <= TELEM_DOWNLOAD) {
            c.ssid[sizeof c.ssid - 1] = 0; c.pass[sizeof c.pass - 1] = 0; c.host[sizeof c.host - 1] = 0; c.token[sizeof c.token - 1] = 0;
            if (c.decim_pt == 0 || c.decim_lc == 0) { c.decim_pt = TELEM_DEFAULT_DECIM_PT; c.decim_lc = TELEM_DEFAULT_DECIM_LC; }
            s_cfg = c;
        }
        nvs_close(h);
    }
    s_run = s_cfg;
    if (s_run.mode == TELEM_OFF) { return; }                   /* OFF: nothing else happens, no memory is used */
    if (!s_run.ssid[0] || !s_run.host[0]) {
        ESP_LOGW(TAG, "mode %u but no SSID or server set: telemetry stays off", (unsigned)s_run.mode);
        return;
    }
    if (s_run.mode == TELEM_UPLOAD) {
        s_ring = heap_caps_malloc(RING_RECS * sizeof(ring_rec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_up = heap_caps_malloc(sizeof *s_up, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring || !s_up) { ESP_LOGE(TAG, "no memory for the ring: telemetry stays off"); return; }
    }
    telem_run_mode = s_run.mode;                               /* last: the sink starts to call telem_push() */
    ESP_LOGI(TAG, "mode %s, WiFi starts after the mount", s_run.mode == TELEM_UPLOAD ? "UPLOAD" : "DOWNLOAD");
}

bool telem_set_mode(telem_mode_t m)
{
    if ((int)m < TELEM_OFF || m > TELEM_DOWNLOAD) { return false; }
    s_cfg.mode = (uint8_t)m;
    return cfg_save();
}

bool telem_set_ssid(const char *ssid)
{
    if (!ssid || strlen(ssid) > 32) { return false; }
    strcpy(s_cfg.ssid, ssid);
    return cfg_save();
}

bool telem_set_pass(const char *pass)
{
    if (!pass || strlen(pass) > 63) { return false; }
    strcpy(s_cfg.pass, pass);
    return cfg_save();
}

bool telem_set_server(const char *host, uint16_t port, const char *token)
{
    if (!host || !*host || strlen(host) > 63 || port == 0 || (token && strlen(token) > TP_TOKEN_LEN)) { return false; }
    strcpy(s_cfg.host, host);
    s_cfg.port = port;
    strcpy(s_cfg.token, token ? token : "");
    return cfg_save();
}

bool telem_set_decim(uint16_t pt, uint16_t lc)
{
    if (pt == 0 || lc == 0) { return false; }
    s_cfg.decim_pt = pt;
    s_cfg.decim_lc = lc;
    return cfg_save();
}

void telem_get_info(telem_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->saved_mode = (telem_mode_t)s_cfg.mode;
    o->run_mode = (telem_mode_t)telem_run_mode;
    strcpy(o->ssid, s_cfg.ssid);
    strcpy(o->host, s_cfg.host);
    o->pass_set = s_cfg.pass[0] != 0;
    o->token_set = s_cfg.token[0] != 0;
    o->port = s_cfg.port;
    o->decim_pt = s_cfg.decim_pt;
    o->decim_lc = s_cfg.decim_lc;
    o->wifi_up = s_wifi_up;
    o->link_up = s_link_up;
    o->rssi = s_rssi;
    o->drops = s_drops;
    o->sent_frames = s_sent_frames;
    o->reconnects = s_reconnects;
}

bool telem_link_up(void) { return s_link_up; }

/* ---------------------------------------------------------------- UPLOAD: the ring */

/* One record into the ring: every Nth of each device and sensor. The short critical section copies 9 bytes. */
static void ring_add(uint8_t dev, const record_t *r)
{
    if (rec_is_meta(r)) { return; }                             /* the task makes its own calibration frames */
    unsigned k = rec_is_lc(r) ? 1U : 0U;
    uint16_t every = k ? s_run.decim_lc : s_run.decim_pt;
    dev &= (NDEV - 1U);
    portENTER_CRITICAL(&s_rmux);
    if (++s_cnt[dev][k] >= every) {
        s_cnt[dev][k] = 0;
        if (s_rhead - s_rtail >= RING_RECS) { s_rtail++; s_drops++; }      /* full: the oldest record goes */
        s_ring[s_rhead & (RING_RECS - 1U)].r = *r;
        s_ring[s_rhead & (RING_RECS - 1U)].dev = dev;
        s_rhead++;
    }
    portEXIT_CRITICAL(&s_rmux);
}

/* The own sensors: called from the sink under its lock. */
void telem_push_slow(const record_t *r)
{
    s_local_data = true;
    ring_add(io_addr(), r);
}

/* Another board: records of a chain packet (called by the chain receive tasks). */
void telem_push_remote_slow(uint8_t dev, const uint8_t *recs, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        record_t r;
        memcpy(&r, recs + i * sizeof r, sizeof r);
        ring_add(dev, &r);
    }
}

void telem_push_remote_cal_slow(uint8_t dev, const uint8_t *recs, unsigned n)
{
    if (n != CAL_BLOCK_RECS) { return; }
    dev &= (NDEV - 1U);
    portENTER_CRITICAL(&s_rmux);
    memcpy(s_rcal[dev].b, recs, sizeof s_rcal[dev].b);
    s_rcal[dev].ver++;
    s_rcal[dev].valid = true;
    portEXIT_CRITICAL(&s_rmux);
}

static unsigned ring_pop(ring_rec_t *out, unsigned max)
{
    unsigned n;
    portENTER_CRITICAL(&s_rmux);
    n = s_rhead - s_rtail;
    if (n > max) { n = max; }
    for (unsigned i = 0; i < n; i++) { out[i] = s_ring[(s_rtail + i) & (RING_RECS - 1U)]; }
    s_rtail += n;
    portEXIT_CRITICAL(&s_rmux);
    return n;
}

static void ring_flush(void)
{
    portENTER_CRITICAL(&s_rmux);
    s_rtail = s_rhead;
    portEXIT_CRITICAL(&s_rmux);
}

static uint32_t ring_count(void)
{
    portENTER_CRITICAL(&s_rmux);
    uint32_t n = s_rhead - s_rtail;
    portEXIT_CRITICAL(&s_rmux);
    return n;
}

/* ---------------------------------------------------------------- WiFi and sockets */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { s_wifi_up = false; }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { s_wifi_up = true; ESP_LOGI(TAG, "WiFi connected"); }
}

static bool wifi_bringup(void)
{
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    if (esp_netif_init() != ESP_OK) { return false; }
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { return false; }
    if (!esp_netif_create_default_wifi_sta()) { return false; }
    if (esp_wifi_init(&icfg) != ESP_OK) { return false; }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);                      /* no flash write of the WiFi settings */
    esp_wifi_set_mode(WIFI_MODE_STA);
    strlcpy((char *)wc.sta.ssid, s_run.ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, s_run.pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = s_run.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_start() != ESP_OK) { return false; }
    esp_wifi_set_ps(WIFI_PS_NONE);                               /* the modem sleep adds delay and lost frames */
    return true;
}

/* Keep trying to connect to the access point. Returns true while WiFi is up. */
static bool ensure_wifi(void)
{
    static int64_t last_try;
    if (s_wifi_up) { return true; }
    int64_t now = esp_timer_get_time();
    if (now - last_try >= 3000000LL || last_try == 0) { last_try = now ? now : 1; (void)esp_wifi_connect(); }
    vTaskDelay(pdMS_TO_TICKS(250));
    return false;
}

static void set_timeouts(int fd, int rcv_ms, int snd_ms)
{
    struct timeval r = { rcv_ms / 1000, (rcv_ms % 1000) * 1000 }, s = { snd_ms / 1000, (snd_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof s);
}

static int tcp_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { return -1; }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        int err = 0;
        socklen_t el = sizeof err;
        fd_set w;
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        FD_ZERO(&w);
        FD_SET(fd, &w);
        if (errno != EINPROGRESS || select(fd + 1, NULL, &w, NULL, &tv) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int w = send(fd, p, n, 0);
        if (w <= 0) { return false; }                            /* a timeout (3 s) counts as an error: reconnect */
        p += w; n -= (size_t)w;
    }
    return true;
}

static bool recv_exact(int fd, uint8_t *p, size_t n)
{
    while (n) {
        int r = recv(fd, p, n, 0);
        if (r <= 0) { return false; }
        p += r; n -= (size_t)r;
    }
    return true;
}

/* Read one frame from the socket. */
static bool rx_frame(int fd, uint8_t *buf, size_t cap, uint8_t *type, const uint8_t **pay, size_t *len)
{
    if (!recv_exact(fd, buf, TP_HDR)) { return false; }
    size_t l = tp_get_u16(buf + 5);
    if (TP_HDR + l + TP_CRC > cap) { return false; }
    if (!recv_exact(fd, buf + TP_HDR, l + TP_CRC)) { return false; }
    return tp_check_frame(buf, TP_HDR + l + TP_CRC, type, pay, len) > 0;
}

/* HELLO and ACK. */
static bool hello(int fd, uint8_t role, uint8_t *buf, size_t cap)
{
    uint8_t pay[sizeof(tp_hello_t)];
    uint8_t type;
    const uint8_t *rp;
    size_t rl;
    memset(pay, 0, sizeof pay);
    pay[0] = role;
    memcpy(pay + 1, s_run.token, strnlen(s_run.token, TP_TOKEN_LEN));
    size_t n = tp_make_frame(buf, 0, TP_HELLO, pay, sizeof pay);
    set_timeouts(fd, 3000, 3000);
    if (!send_all(fd, buf, n)) { return false; }
    if (!rx_frame(fd, buf, cap, &type, &rp, &rl)) { return false; }
    if (type == TP_NAK) { ESP_LOGE(TAG, "server refused (reason %u): check the token", rl ? (unsigned)rp[0] : 0U); return false; }
    return type == TP_ACK;
}

static void update_rssi(void)
{
    wifi_ap_record_t ap;
    s_rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

/* ---------------------------------------------------------------- UPLOAD task */


/* One frame for each device and sensor that has records. Each frame has the device ID of the board that made the data. */
static bool up_send_batches(int fd)
{
    for (unsigned d = 0; d < NDEV; d++) {
        for (int k = 0; k < 2; k++) {
            unsigned *cnt = k ? &s_up->nlc[d] : &s_up->npt[d];
            if (!*cnt) { continue; }
            size_t n = tp_make_frame(s_up->out, (uint8_t)d, k ? TP_DATA_LC : TP_DATA_PT, k ? (const void *)s_up->lc[d] : (const void *)s_up->pt[d],
                                     *cnt * sizeof(record_t));
            *cnt = 0;
            s_sent_frames++;
            if (!send_all(fd, s_up->out, n)) { return false; }
        }
    }
    return true;
}

static void up_clear(void)
{
    memset(s_up->npt, 0, sizeof s_up->npt);
    memset(s_up->nlc, 0, sizeof s_up->nlc);
}

static void upload_session(int fd)
{
    const uint8_t self = (uint8_t)(io_addr() & (NDEV - 1U));
    int64_t last_send = esp_timer_get_time(), last_status = 0, last_cal = 0;
    uint32_t cal_rev = 0xFFFFFFFFU, rcal_sent[NDEV];
    ring_rec_t tmp[16];
    for (unsigned d = 0; d < NDEV; d++) { rcal_sent[d] = 0xFFFFFFFFU; }
    for (;;) {
        unsigned n;
        do {                                                    /* the ring into the batches */
            n = ring_pop(tmp, 16);
            for (unsigned i = 0; i < n; i++) {
                unsigned d = tmp[i].dev & (NDEV - 1U);
                if (rec_is_lc(&tmp[i].r)) {
                    if (s_up->nlc[d] == BATCH_RECS && !up_send_batches(fd)) { return; }
                    s_up->lc[d][s_up->nlc[d]++] = tmp[i].r;
                } else {
                    if (s_up->npt[d] == BATCH_RECS && !up_send_batches(fd)) { return; }
                    s_up->pt[d][s_up->npt[d]++] = tmp[i].r;
                }
            }
        } while (n == 16);

        int64_t now = esp_timer_get_time();
        if (now - last_send >= (int64_t)SEND_MS * 1000) {
            if (!up_send_batches(fd)) { return; }
            last_send = now;
        }
        cal_t c;
        cal_get(&c);
        if (s_local_data && (c.rev != cal_rev || now - last_cal >= (int64_t)CAL_MS * 1000)) {   /* own calibration: at the start, when it changes, every 30 s */
            record_t b[CAL_BLOCK_RECS];
            cal_make_block(b);
            size_t m = tp_make_frame(s_up->out, self, TP_CAL, b, sizeof b);
            if (!send_all(fd, s_up->out, m)) { return; }
            cal_rev = c.rev;
            last_cal = now;
        }
        for (unsigned d = 0; d < NDEV; d++) {                   /* the calibration blocks of the other boards: each time one comes (they repeat every 30 s) */
            record_t b[CAL_BLOCK_RECS];
            bool send = false;
            portENTER_CRITICAL(&s_rmux);
            if (s_rcal[d].valid && s_rcal[d].ver != rcal_sent[d]) { memcpy(b, s_rcal[d].b, sizeof b); rcal_sent[d] = s_rcal[d].ver; send = true; }
            portEXIT_CRITICAL(&s_rmux);
            if (send) {
                size_t m = tp_make_frame(s_up->out, (uint8_t)d, TP_CAL, b, sizeof b);
                if (!send_all(fd, s_up->out, m)) { return; }
            }
        }
        if (now - last_status >= (int64_t)STATUS_MS * 1000) {
            tp_status_t st;
            update_rssi();
            st.rssi = (int8_t)s_rssi;
            st.flags = 1U;
            st.drops = s_drops;
            st.queue = ring_count();
            st.uptime_s = (uint32_t)(now / 1000000);
            st.reconnects = (uint16_t)s_reconnects;
            size_t m = tp_make_frame(s_up->out, self, TP_STATUS, &st, sizeof st);
            if (!send_all(fd, s_up->out, m)) { return; }
            last_status = now;
        }
        if (!s_wifi_up) { return; }
        uint8_t junk[32];                                       /* the server sends nothing after the ACK; a close or a NAK ends the session */
        int r = recv(fd, junk, sizeof junk, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) { return; }
        if (r > 0 && r >= 5 && junk[4] == TP_NAK) { ESP_LOGE(TAG, "server sent NAK"); return; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void upload_task(void *arg)
{
    uint32_t backoff = 1000;
    (void)arg;
    for (;;) {
        if (!ensure_wifi()) { continue; }
        int fd = tcp_connect(s_run.host, s_run.port, 5000);
        if (fd >= 0 && hello(fd, TP_ROLE_UPLOAD, s_up->out, sizeof s_up->out)) {
            backoff = 1000;
            if (s_had_link) { s_reconnects++; }
            s_had_link = true;
            ring_flush();                                       /* only the present: old data waits in the NAND and SD files */
            up_clear();
            s_link_up = true;
            ESP_LOGI(TAG, "upload link up");
            set_timeouts(fd, 100, 3000);
            upload_session(fd);
            s_link_up = false;
            ESP_LOGW(TAG, "upload link down");
        }
        if (fd >= 0) { close(fd); }
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff < 10000U ? backoff * 2U : 10000U;
    }
}

/* ---------------------------------------------------------------- DOWNLOAD task */

static int s_rocket_rssi;
static uint32_t s_rocket_drops;

static void emit(const record_t *r) { (void)stream_push_wait(r, 500); }

/* One BATCH: next cursor, age, and whole stored frames. The records go to the stream buffer; stream.c prints them. */
static bool handle_batch(const uint8_t *pay, size_t len, uint64_t *next)
{
    if (len < 12) { return false; }
    *next = tp_get_u64(pay);
    uint32_t age = tp_get_u32(pay + 8);
    size_t pos = 12;
    uint32_t first[2] = { 0, 0 }, last[2] = { 0, 0 }, cnt[2] = { 0, 0 };     /* time of the first and last record and their number: [0] PT, [1] LC */
    while (pos < len) {
        uint8_t type;
        const uint8_t *p;
        size_t pl;
        int used = tp_check_frame(pay + pos, len - pos, &type, &p, &pl);
        if (used <= 0) { return false; }
        pos += (size_t)used;
        if (type == TP_DATA_PT || type == TP_DATA_LC || type == TP_CAL) {
            for (size_t i = 0; i + TP_REC_SIZE <= pl; i += TP_REC_SIZE) {
                record_t r;
                memcpy(&r, p + i, sizeof r);
                if (type != TP_CAL) {
                    int k = (type == TP_DATA_LC) ? 1 : 0;
                    if (cnt[k]++ == 0) { first[k] = r.timestamp_us; }
                    last[k] = r.timestamp_us;
                }
                emit(&r);
            }
        } else if (type == TP_STATUS && pl == sizeof(tp_status_t)) {
            tp_status_t st;
            memcpy(&st, p, sizeof st);
            s_rocket_rssi = st.rssi;
            s_rocket_drops = st.drops;
        }
    }
    update_rssi();
    static uint32_t dt[2];                                      /* the last interval stays when a batch has one record only */
    for (int k = 0; k < 2; k++) { if (cnt[k] > 1) { dt[k] = (last[k] - first[k]) / (cnt[k] - 1U); } }
    record_t dp = rec_make_meta(TELEM_META_DTPT, dt[0]), dl = rec_make_meta(TELEM_META_DTLC, dt[1]);
    emit(&dp);
    emit(&dl);
    record_t a = rec_make_meta(TELEM_META_AGE, age);
    uint32_t drops = s_rocket_drops > 0xFFFFU ? 0xFFFFU : s_rocket_drops;
    record_t l = rec_make_meta(TELEM_META_LINK, drops | ((uint32_t)(uint8_t)(int8_t)s_rocket_rssi << 16) | ((uint32_t)(uint8_t)(int8_t)s_rssi << 24));
    emit(&l);
    emit(&a);                                                   /* last: the end of the batch */
    return true;
}

static void download_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(DL_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t backoff = 1000;
    uint64_t cursor = TP_CURSOR_NOW;
    int64_t last_ok = 0;
    (void)arg;
    if (!buf) { ESP_LOGE(TAG, "no memory for the download buffer"); vTaskDelete(NULL); return; }
    for (;;) {
        if (!ensure_wifi()) { continue; }
        int fd = tcp_connect(s_run.host, s_run.port, 5000);
        if (fd >= 0 && hello(fd, TP_ROLE_DOWNLOAD, buf, DL_BUF)) {
            backoff = 1000;
            if (s_had_link) { s_reconnects++; }
            s_had_link = true;
            if (esp_timer_get_time() - last_ok > RESUME_MAX_US) { cursor = TP_CURSOR_NOW; }
            s_link_up = true;
            ESP_LOGI(TAG, "download link up");
            for (;;) {
                uint8_t g[sizeof(tp_get_t)], type;
                const uint8_t *p;
                size_t pl, n;
                tp_put_u64(g, cursor); tp_put_u16(g + 8, GET_BYTES); tp_put_u16(g + 10, GET_WAIT_MS);
                n = tp_make_frame(buf, 0, TP_GET, g, sizeof g);
                set_timeouts(fd, GET_WAIT_MS + 4000, 3000);
                if (!send_all(fd, buf, n) || !rx_frame(fd, buf, DL_BUF, &type, &p, &pl) || type != TP_BATCH) { break; }
                if (!handle_batch(p, pl, &cursor)) { break; }
                last_ok = esp_timer_get_time();
                if (!s_wifi_up) { break; }
            }
            s_link_up = false;
            ESP_LOGW(TAG, "download link down");
        }
        if (fd >= 0) { close(fd); }
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff < 10000U ? backoff * 2U : 10000U;
    }
}

/* ---------------------------------------------------------------- start */

void telem_start(void)
{
    if (telem_run_mode == TELEM_OFF) { return; }
    if (!wifi_bringup()) {
        ESP_LOGE(TAG, "WiFi does not start: telemetry stays off");
        telem_run_mode = TELEM_OFF;
        return;
    }
    /* Core 0, as the WiFi task. The sensor ISR of the ADS1115 and the PFS writer are on core 1 and are not touched. */
    xTaskCreatePinnedToCore(telem_run_mode == TELEM_UPLOAD ? upload_task : download_task, "telem", 6144, NULL, 2, NULL, 0);
}
