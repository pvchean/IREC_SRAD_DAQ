#include "cmd.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "chain.h"
#include "io.h"
#include "led_sr.h"
#include "record.h"
#include "sd_log.h"
#include "stream.h"

#define CHUNK 1024U                      /* multiple of sizeof(record_t) */
#define FRAME 4096U                      /* GET frame size, multiple of sizeof(record_t) */
#define SEEN_MS 3000U                    /* DAQS: a board is in the chain if it sent in this time */
#define IDLE_MS 500                      /* a line without a line end runs after this idle time (as the sketch) */

static pfs_ctx_t *s_pfs;

static void do_info(void)
{
    pfs_status_t st;
    chain_stats_t cs;
    chain_get_stats(&cs);
    pfs_get_status(s_pfs, &st);
    printf("STATUS:OK rec=%d auto=%d addr=%u mounted=%d path=%u index=%d file=%u ingested=%" PRIu64 " dropped=%" PRIu64
           " pages=%" PRIu32 " degraded=%d no_storage=%d nor_failed=%d verify_fail=%" PRIu32 " rs_repairs=%" PRIu32
           " usable_blocks=%u retired=%u sd=%d sd_bytes=%" PRIu64 " sd_drop=%" PRIu32 " sd_err=%" PRIu32 " up_s=%" PRIu64 "\n",
           rec_get(), io_auto_mode(), io_addr(), st.mounted, st.mount_path, st.index_ready, st.active_file_id,
           st.ingested_bytes, st.dropped_bytes, st.pages_written, st.degraded, st.no_storage, st.nor_failed,
           st.verify_failures, st.rs_repairs, st.usable_blocks, st.retired_blocks,
           sd_log_mounted(), sd_log_bytes(), sd_log_dropped(), sd_log_errors(),
           (uint64_t)(esp_timer_get_time() / 1000000));
    printf("CHAIN: tx=%" PRIu32 " rx1=%" PRIu32 "/%" PRIu32 " rx2=%" PRIu32 "/%" PRIu32 " (ok/bad) fwd=%" PRIu32
           " tx_drop=%" PRIu32 " own_dropped=%" PRIu32 "\n",
           cs.tx_packets, cs.rx_ok[0], cs.rx_bad[0], cs.rx_ok[1], cs.rx_bad[1], cs.forwarded, cs.tx_drop, cs.own_dropped);
    uint8_t fl[8], m = chain_seen(SEEN_MS, fl);
    for (int i = 0; i < 8; i++) {                  /* heartbeat flags: 1 rec, 2 auto, 4 sensor, 8 SD */
        if (m & (1U << i)) { printf("DAQ %d flags=0x%02X\n", i, fl[i]); }
    }
}

/* List the files on the SD card with their sizes. */
static void do_sdls(void)
{
    DIR *d = sd_log_mounted() ? opendir("/sdcard") : NULL;
    struct dirent *e;
    if (!d) { printf("SDLS:ERR no card\n"); return; }
    while ((e = readdir(d)) != NULL) {
        struct stat sb;
        char p[300];
        snprintf(p, sizeof p, "/sdcard/%s", e->d_name);
        printf("SDFILE %s %ld\n", e->d_name, stat(p, &sb) == 0 ? (long)sb.st_size : -1L);
    }
    closedir(d);
    printf("SDLS:END\n");
}

/* DAQS: the boards in the chain. One line, one USB write, so it stays whole between the number lines. */
static void do_daqs(void)
{
    char line[64];
    int len = snprintf(line, sizeof line, "DAQS:SELF=%u SEEN=", io_addr());
    uint8_t m = chain_seen(SEEN_MS, NULL);
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (m & (1U << i)) { len += snprintf(line + len, sizeof line - len, first ? "%d" : ",%d", i); first = false; }
    }
    len += snprintf(line + len, sizeof line - len, "\r\n");
    fflush(stdout);
    usb_serial_jtag_write_bytes(line, (size_t)len, pdMS_TO_TICKS(50));
}

/* FILES: the PFS files and the SD files, one line each, for the MATLAB file list.
     FILE PFS <file id> <bytes> <flags hex>     bytes = stored bytes (approximate if flag 0x0004 is clear)
     FILE SD <name> <bytes> */
static void do_files(void)
{
    uint16_t n = pfs_file_count(s_pfs);
    bool was_on = stream_enabled();
    stream_enable(false);                          /* no number lines inside the file lines */
    printf("FILES:BEGIN\n");
    for (uint16_t i = 0; i < n; i++) {
        pfs_file_info_t fi;
        if (pfs_file_info(s_pfs, i, &fi) == PFS_OK) {
            printf("FILE PFS %u %" PRIu64 " %04X\n", fi.file_id,
                   fi.size_bytes > fi.first_offset ? fi.size_bytes - fi.first_offset : 0U, fi.flags);
        }
    }
    DIR *d = sd_log_mounted() ? opendir("/sdcard") : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        struct stat sb;
        char p[300];
        snprintf(p, sizeof p, "/sdcard/%s", e->d_name);
        if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) { printf("FILE SD %s %ld\n", e->d_name, (long)sb.st_size); }
    }
    if (d) { closedir(d); }
    printf("FILES:END\n");
    fflush(stdout);
    if (was_on) { stream_enable(true); }
}

/* ---- GET: send one file in binary frames ----
     GET PFS <file id>     the records of a PFS file (from the first whole record)
     GET SD <name>         an SD file as it is, for example GET SD F00012D3.BIN
   Reply: the line "GET:BEGIN <source> <name> <approximate bytes>", then frames [u16 len, little endian]
   [len bytes], then a frame with len 0, then the line "GET:END bytes=<n> crc32=<8 hex> OK|ERR".
   crc32 = the zlib CRC-32 of all the bytes. An error before the first frame gives "GET:ERR <reason>".
   The live stream stops and the log is muted while the file goes out. The stream starts again after. */
static uint8_t s_frame[2 + FRAME];

/* The driver puts a write in its TX ring buffer (4096 bytes) in one piece or not at all: a write larger than
   the ring buffer always fails. So write in pieces of USB_PIECE bytes. */
#define USB_PIECE 1024U

static bool usb_put(const uint8_t *p, size_t n)
{
    while (n) {
        size_t k = n < USB_PIECE ? n : USB_PIECE;
        if (usb_serial_jtag_write_bytes(p, k, pdMS_TO_TICKS(3000)) != (int)k) { return false; }   /* the host does not read */
        p += k; n -= k;
    }
    return true;
}

static bool send_frame(size_t len, uint32_t *crc, uint64_t *total)
{
    s_frame[0] = (uint8_t)len; s_frame[1] = (uint8_t)(len >> 8);
    if (!usb_put(s_frame, 2 + len)) { return false; }
    *crc = esp_rom_crc32_le(*crc, s_frame + 2, (uint32_t)len);   /* count only the bytes that went out */
    *total += len;
    return true;
}

static void do_get(const char *src, const char *name)
{
    bool pfs = src && !strcmp(src, "PFS"), sd = src && !strcmp(src, "SD");
    bool was_on = stream_enabled(), ok = true;
    uint32_t crc = 0;
    uint64_t total = 0;
    pfs_file_info_t fi;
    FILE *f = NULL;
    uint64_t off = 0;
    char path[48];

    if (!name || (!pfs && !sd)) { printf("GET:ERR use GET PFS <id> or GET SD <name>\n"); return; }
    if (pfs) {
        if (pfs_file_stat(s_pfs, (uint16_t)atoi(name), &fi) != PFS_OK) { printf("GET:ERR no PFS file %s\n", name); return; }
        off = (fi.first_offset + sizeof(record_t) - 1) / sizeof(record_t) * sizeof(record_t);
    } else {
        if (strchr(name, '/') || strlen(name) > 12) { printf("GET:ERR bad name\n"); return; }
        (void)sd_log_flush(2000);                       /* write the staged bytes and close the files */
        snprintf(path, sizeof path, "/sdcard/%s", name);
        f = sd_log_mounted() ? fopen(path, "rb") : NULL;
        if (!f) { printf("GET:ERR no SD file %s\n", name); return; }
    }
    stream_enable(false);
    esp_log_level_set("*", ESP_LOG_NONE);              /* a log line in the frames would break them */
    if (pfs) { printf("GET:BEGIN PFS %u %" PRIu64 "\n", fi.file_id, fi.size_bytes > off ? fi.size_bytes - off : 0U); }
    else { fseek(f, 0, SEEK_END); printf("GET:BEGIN SD %s %ld\n", name, ftell(f)); fseek(f, 0, SEEK_SET); }
    fflush(stdout);

    for (;;) {
        size_t got = 0;
        if (pfs) {
            pfs_err_t e = pfs_read(s_pfs, fi.file_id, off, s_frame + 2, FRAME, &got);
            if (e == PFS_ERR_EOF) { break; }
            if (e != PFS_OK) { ok = false; break; }
            got -= got % sizeof(record_t);              /* whole records; the rest comes in the next read */
            off += got;
        } else {
            got = fread(s_frame + 2, 1, FRAME, f);
            if (ferror(f)) { ok = false; }
        }
        if (got == 0) { break; }
        if (!send_frame(got, &crc, &total)) { ok = false; break; }
        vTaskDelay(1);                                   /* give the other tasks time */
    }
    if (f) { fclose(f); }
    s_frame[0] = s_frame[1] = 0;
    if (!usb_put(s_frame, 2)) { ok = false; }
    printf("GET:END bytes=%" PRIu64 " crc32=%08" PRIX32 " %s\n", total, crc, ok ? "OK" : "ERR");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
    if (was_on) { stream_enable(true); }
}

static void do_list(void)
{
    uint16_t n = pfs_file_count(s_pfs);
    printf("LIST:%u files\n", n);
    for (uint16_t i = 0; i < n; i++) {
        pfs_file_info_t fi;
        if (pfs_file_info(s_pfs, i, &fi) == PFS_OK) {
            printf("FILE id=%u flags=0x%04X pages=%" PRIu32 " first_offset=%" PRIu64 " size=%" PRIu64 "\n",
                   fi.file_id, fi.flags, fi.pages, fi.first_offset, fi.size_bytes);
        }
    }
    printf("LIST:END\n");
}

/* Decode records and test: seq steps by 1, the time goes forward, the interval is about 1.16 ms (860 SPS). */
static void do_check(int file_id, uint32_t max_rec)
{
    static uint8_t buf[CHUNK];
    pfs_status_t st;
    pfs_file_info_t fi;
    pfs_get_status(s_pfs, &st);
    if (file_id < 0) { file_id = st.active_file_id; }
    if (pfs_file_stat(s_pfs, (uint16_t)file_id, &fi) != PFS_OK) { printf("CHECK:ERR no file %d\n", file_id); return; }

    uint64_t off = (fi.first_offset + sizeof(record_t) - 1) / sizeof(record_t) * sizeof(record_t);
    uint32_t n = 0, breaks = 0, missing = 0, back = 0, read_err = 0;
    uint32_t prev_ts = 0, max_gap = 0, near = 0;
    uint64_t near_sum = 0, sum = 0;
    uint16_t prev_seq = 0, first_seq = 0, last_seq = 0;
    int16_t rmin = INT16_MAX, rmax = INT16_MIN;
    uint32_t first_ts = 0;

    while (n < max_rec) {
        size_t got = 0;
        pfs_err_t e = pfs_read(s_pfs, (uint16_t)file_id, off, buf, CHUNK, &got);
        if (e == PFS_ERR_EOF) { break; }
        if (e != PFS_OK) { read_err++; break; }
        if (got < sizeof(record_t)) { break; }
        for (size_t i = 0; i + sizeof(record_t) <= got && n < max_rec; i += sizeof(record_t)) {
            record_t r;
            memcpy(&r, buf + i, sizeof r);
            if (n == 0) { first_seq = r.seq; first_ts = r.timestamp_us; }
            else {
                uint16_t ds = (uint16_t)(r.seq - prev_seq);
                uint32_t dt = r.timestamp_us - prev_ts;
                if (ds != 1) { breaks++; missing += (uint16_t)(ds - 1U); }
                if (dt > 0x80000000U) { back++; }
                else {
                    if (dt > max_gap) { max_gap = dt; }
                    if (dt < 10000U) { near++; near_sum += dt; }
                }
            }
            if (r.reading < rmin) { rmin = r.reading; }
            if (r.reading > rmax) { rmax = r.reading; }
            sum += (uint64_t)(int64_t)r.reading + 40000U;
            prev_seq = last_seq = r.seq; prev_ts = r.timestamp_us; n++;
        }
        off += got - got % sizeof(record_t);
        vTaskDelay(1);                       /* give the other tasks time */
    }
    printf("CHECK:%s file=%d records=%" PRIu32 " seq_breaks=%" PRIu32 " missing=%" PRIu32 " time_back=%" PRIu32
           " read_errors=%" PRIu32 " seq=%u..%u span_us=%" PRIu32 " mean_interval_us=%" PRIu32 " max_gap_us=%" PRIu32
           " reading_min=%d reading_max=%d reading_mean=%" PRId64 "\n",
           (n && !read_err && !back) ? "OK" : "WARN", file_id, n, breaks, missing, back, read_err, first_seq, last_seq,
           prev_ts - first_ts, near ? (uint32_t)(near_sum / near) : 0U, max_gap,
           n ? rmin : 0, n ? rmax : 0, n ? (int64_t)(sum / n) - 40000 : 0);
}

static void do_dump(int file_id, uint64_t off, size_t len)
{
    uint8_t b[256];
    size_t got = 0;
    if (len > sizeof b) { len = sizeof b; }
    pfs_err_t e = pfs_read(s_pfs, (uint16_t)file_id, off, b, len, &got);
    if (e != PFS_OK && e != PFS_ERR_EOF) { printf("DUMP:ERR %d\n", (int)e); return; }
    printf("DUMP file=%d offset=%" PRIu64 " len=%u ", file_id, off, (unsigned)got);
    for (size_t i = 0; i < got; i++) { printf("%02X", b[i]); }
    printf("\n");
}


/* ---- COMPARE: the own data in PFS against the data of the other board on the SD card ----
   Both boards read the same square wave on the sensor line. Their clocks and sample instants differ, so the
   records are not equal byte by byte. The test finds the edges of the wave in both files and compares the
   time between the edges and the two levels. It also tests the remote file for gaps (the chain test). */
#define EDGE_MAX    200
#define HI_ON       12000          /* counts: above = high */
#define LO_ON       5000           /* counts: below = low */

typedef struct {
    uint32_t n_rec, seq_breaks, missing;
    uint32_t edges;                 /* rising edges found */
    uint32_t t[EDGE_MAX];           /* time of each rising edge, us */
    uint64_t hi_sum, lo_sum;
    uint32_t hi_n, lo_n;
} wave_t;

static void wave_add(wave_t *w, const record_t *r, bool *high, bool *have_prev, uint16_t *pseq)
{
    if (*have_prev) {
        uint16_t ds = (uint16_t)(r->seq - *pseq);
        if (ds != 1) { w->seq_breaks++; w->missing += (uint16_t)(ds - 1U); }
    }
    *have_prev = true; *pseq = r->seq; w->n_rec++;
    if (r->reading > HI_ON) {
        if (!*high && w->edges < EDGE_MAX && w->n_rec > 1) { w->t[w->edges++] = r->timestamp_us; }
        *high = true; w->hi_sum += (uint64_t)r->reading; w->hi_n++;
    } else if (r->reading < LO_ON) {
        *high = false; w->lo_sum += (uint64_t)(r->reading + 1000); w->lo_n++;   /* +1000: stay positive */
    }
}

static bool scan_pfs(uint16_t file_id, wave_t *w)
{
    static uint8_t buf[CHUNK];
    pfs_file_info_t fi;
    bool high = false, have = false;
    uint16_t pseq = 0;
    if (pfs_file_stat(s_pfs, file_id, &fi) != PFS_OK) { return false; }
    uint64_t off = (fi.first_offset + sizeof(record_t) - 1) / sizeof(record_t) * sizeof(record_t);
    for (;;) {
        size_t got = 0;
        if (pfs_read(s_pfs, file_id, off, buf, CHUNK, &got) != PFS_OK || got < sizeof(record_t)) { break; }
        for (size_t i = 0; i + sizeof(record_t) <= got; i += sizeof(record_t)) {
            record_t r;
            memcpy(&r, buf + i, sizeof r);
            wave_add(w, &r, &high, &have, &pseq);
        }
        off += got - got % sizeof(record_t);
        if (w->edges >= EDGE_MAX) { break; }
        vTaskDelay(1);
    }
    return true;
}

static bool scan_sd(const char *path, wave_t *w)
{
    static uint8_t buf[CHUNK];
    bool high = false, have = false;
    uint16_t pseq = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { return false; }
    size_t got;
    while (w->edges < EDGE_MAX && (got = fread(buf, 1, CHUNK, f)) >= sizeof(record_t)) {
        for (size_t i = 0; i + sizeof(record_t) <= got; i += sizeof(record_t)) {
            record_t r;
            memcpy(&r, buf + i, sizeof r);
            wave_add(w, &r, &high, &have, &pseq);
        }
        vTaskDelay(1);
    }
    fclose(f);
    return true;
}

static void print_wave(const char *name, const wave_t *w)
{
    printf("  %s: records=%" PRIu32 " seq_breaks=%" PRIu32 " missing=%" PRIu32 " edges=%" PRIu32
           " high_mean=%" PRIu32 " low_mean=%" PRId32 "\n", name, w->n_rec, w->seq_breaks, w->missing, w->edges,
           w->hi_n ? (uint32_t)(w->hi_sum / w->hi_n) : 0U, w->lo_n ? (int32_t)(w->lo_sum / w->lo_n) - 1000 : 0);
}

static void do_compare(int remote)
{
    static wave_t a, b;                               /* a = own data from PFS, b = remote data from SD */
    pfs_status_t st;
    char path[40];
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    if (remote < 0 || remote > 7 || remote == (int)io_addr()) { printf("COMPARE:ERR give the ID of the other board\n"); return; }
    if (!sd_log_flush(3000)) { printf("COMPARE:ERR the SD card does not flush\n"); return; }
    pfs_get_status(s_pfs, &st);
    /* The SD file of the other board: its sensor data, sent over UART, saved under its ID. */
    snprintf(path, sizeof path, "/sdcard/F%05uD%d.BIN", (unsigned)st.active_file_id, remote);
    printf("COMPARE: PFS file %u (own, ID %u) against %s (ID %d, over UART)\n",
           (unsigned)st.active_file_id, io_addr(), path, remote);
    if (!scan_pfs(st.active_file_id, &a)) { printf("COMPARE:ERR no PFS file\n"); return; }
    if (!scan_sd(path, &b)) { printf("COMPARE:ERR no SD file %s\n", path); return; }
    print_wave("PFS ", &a);
    print_wave("SD  ", &b);

    uint32_t n = a.edges < b.edges ? a.edges : b.edges;
    if (n < 4) { printf("COMPARE:FAIL fewer than 4 edges (PFS %" PRIu32 ", SD %" PRIu32 "). Is the wave on the sensor line?\n", a.edges, b.edges); return; }
    /* The files can start at different edges. Try each shift of the SD edges (0..3) and keep the best one. */
    uint32_t best_bad = UINT32_MAX, best_shift = 0, best_cnt = 0;
    int32_t worst = 0;
    for (uint32_t shift = 0; shift < 4; shift++) {
        for (int dir = 0; dir < 2; dir++) {
            uint32_t sa = dir ? shift : 0, sb = dir ? 0 : shift;
            uint32_t cnt = (a.edges - sa < b.edges - sb ? a.edges - sa : b.edges - sb);
            uint32_t bad = 0;
            int32_t w = 0;
            for (uint32_t i = 1; i < cnt; i++) {
                int32_t da = (int32_t)(a.t[sa + i] - a.t[sa + i - 1]), db = (int32_t)(b.t[sb + i] - b.t[sb + i - 1]);
                int32_t d = da > db ? da - db : db - da;
                if (d > w) { w = d; }
                if (d > 4000) { bad++; }              /* 4 ms: two sample intervals */
            }
            if (cnt >= 4 && bad < best_bad) { best_bad = bad; best_shift = dir ? shift : (uint32_t)-(int32_t)shift; best_cnt = cnt; worst = w; }
        }
    }
    uint32_t span_a = a.t[a.edges - 1] - a.t[0], span_b = b.t[b.edges - 1] - b.t[0];
    int32_t hi_diff = (int32_t)(a.hi_n ? a.hi_sum / a.hi_n : 0) - (int32_t)(b.hi_n ? b.hi_sum / b.hi_n : 0);
    int32_t lo_diff = (int32_t)(a.lo_n ? a.lo_sum / a.lo_n : 0) - (int32_t)(b.lo_n ? b.lo_sum / b.lo_n : 0);
    bool ok = best_bad == 0 && (hi_diff > -400 && hi_diff < 400) && (lo_diff > -400 && lo_diff < 400) &&
              b.seq_breaks == 0 && a.seq_breaks == 0;
    printf("  edge intervals compared=%" PRIu32 " over_4ms=%" PRIu32 " worst_diff_us=%" PRId32 " edge_shift=%" PRId32
           " span_pfs_us=%" PRIu32 " span_sd_us=%" PRIu32 " high_diff=%" PRId32 " low_diff=%" PRId32 "\n",
           best_cnt - 1, best_bad, worst, (int32_t)best_shift, span_a, span_b, hi_diff, lo_diff);
    printf("COMPARE:%s\n", ok ? "PASS the SD copy of the other board and the PFS data show the same wave" : "FAIL see the values above");
}

static void run(char *line)
{
    char *tok = strtok(line, " \t");
    if (!tok) { return; }
    if (!strcmp(tok, "PING")) { printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n"); }
    else if (!strcmp(tok, "MATLAB")) {            /* MATLAB PING: banner, then one voltage per line */
        char *a = strtok(NULL, " \t");
        if (a && !strcmp(a, "PING")) { printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n"); fflush(stdout); stream_enable(true); }
    }
    else if (!strcmp(tok, "LED")) {               /* LED <0..255>: brightness of all LEDs */
        char *a = strtok(NULL, " \t");
        if (a) { long v = strtol(a, NULL, 10); led_set_brightness((uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v)); }
        printf("LED:%u\n", led_get_brightness());
    }
    else if (!strcmp(tok, "STREAM")) {             /* STREAM ON | STREAM OFF */
        char *a = strtok(NULL, " \t");
        stream_enable(a && !strcmp(a, "ON"));
        if (!stream_enabled()) { printf("STREAM:OFF\n"); }
    }
    else if (!strcmp(tok, "DAQS")) { do_daqs(); }
    else if (!strcmp(tok, "FILES")) { do_files(); }
    else if (!strcmp(tok, "GET")) { char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL; do_get(a, b); }
    else if (!strcmp(tok, "STATUS")) { printf("STATUS:OK (SD Overflows: %lu)\n", (unsigned long)sd_log_dropped()); }   /* as the sketch */
    else if (!strcmp(tok, "INFO")) { do_info(); }
    else if (!strcmp(tok, "LIST")) { do_list(); }
    else if (!strcmp(tok, "SDLS")) { do_sdls(); }
    else if (!strcmp(tok, "COMPARE")) { char *a = strtok(NULL, " \t"); do_compare(a ? atoi(a) : -1); }
    else if (!strcmp(tok, "REC")) {
        char *a = strtok(NULL, " \t");
        if (a && !strcmp(a, "ON")) { rec_set(true); } else if (a && !strcmp(a, "OFF")) { rec_set(false); }
        printf("REC:%s\n", rec_get() ? "ON" : "OFF");
    }
    else if (!strcmp(tok, "FLUSH")) { printf(sd_log_flush(2000) ? "SD_FLUSHED:OK\n" : "SD_FLUSHED:ERR\n"); }   /* as the sketch */
    else if (!strcmp(tok, "PFSFLUSH")) { printf("PFSFLUSH:%s\n", pfs_flush(s_pfs, 2000) == PFS_OK ? "OK" : "ERR"); }
    else if (!strcmp(tok, "CHECK")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL;
        do_check(a ? atoi(a) : -1, b ? (uint32_t)strtoul(b, NULL, 10) : 200000U);
    }
    else if (!strcmp(tok, "DUMP")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL, *c = b ? strtok(NULL, " \t") : NULL;
        if (c) { do_dump(atoi(a), strtoull(b, NULL, 10), (size_t)strtoul(c, NULL, 10)); } else { printf("DUMP:ERR args\n"); }
    }
    else { printf("ERR unknown command\n"); }
    fflush(stdout);
}

static void cmd_task(void *arg)
{
    char line[96];
    size_t n = 0;
    (void)arg;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(IDLE_MS)) != 1) {
            if (n) { line[n] = 0; n = 0; run(line); }   /* MATLAB writes "MATLAB PING" without a line end */
            continue;
        }
        if (c == '\n' || c == '\r') { line[n] = 0; n = 0; run(line); }
        else if (n < sizeof line - 1) { line[n++] = (char)c; }
    }
}

void cmd_start(pfs_ctx_t *pfs)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    s_pfs = pfs;
    cfg.tx_buffer_size = 4096;                     /* GET frames */
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(cmd_task, "cmd", 8192, NULL, 1, NULL, 0);
}
