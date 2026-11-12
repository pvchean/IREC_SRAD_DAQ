#include "cmd.h"

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cal.h"
#include "chain.h"
#include "console_io.h"
#include "files_mode.h"
#include "files_mode_cmd.h"
#include "io.h"
#include "led_sr.h"
#include "mtp_store.h"
#include "mtp_store_pfs.h"
#ifdef DAQ_MTP_USB
#include "mtp_vfs.h"
#include "usb_files.h"
#endif
#include "nau7802.h"
#include "profile.h"
#include "record.h"
#include "sd_log.h"
#include "stream.h"
#include "telem.h"

#define CHUNK 1024U                     /* multiple of sizeof(record_t) */
#define SEEN_MS 3000U                    /* DAQS: a board is in the chain if it sent in this time */
#define IDLE_MS 500                      /* a line without a line end runs after this idle time (as the sketch) */

static pfs_ctx_t *s_pfs;

static void cal_reply(const char *head);

/* The SD card is mounted by sd_log.c in serial mode, and by mtp_store_sd.c in files mode (the same path /sdcard). */
static bool sd_up(void)
{
#ifdef DAQ_MTP_USB
    /* REVIEW (MTP-V-5): under the lock of the MTP shim. A direct present() could mount or unmount the card while the USB task reads it. */
    if (files_mode_active()) { return mtp_vfs_sd_present(); }
#else
    if (files_mode_active()) { const mtp_store_t *s = mtp_store_sd_get(); return s && s->present(); }
#endif
    return sd_log_mounted();
}

static void do_info(void)
{
    pfs_status_t st;
    chain_stats_t cs;
    chain_get_stats(&cs);
    pfs_get_status(s_pfs, &st);
    cio_printf("STATUS:OK rec=%d auto=%d addr=%u mounted=%d path=%u index=%d file=%u ingested=%" PRIu64 " dropped=%" PRIu64
           " pages=%" PRIu32 " degraded=%d no_storage=%d nor_failed=%d verify_fail=%" PRIu32 " rs_repairs=%" PRIu32
           " usable_blocks=%u retired=%u sd=%d sd_bytes=%" PRIu64 " sd_drop=%" PRIu32 " sd_err=%" PRIu32 " up_s=%" PRIu64 "\n",
           rec_get(), io_auto_mode(), io_addr(), st.mounted, st.mount_path, st.index_ready, st.active_file_id,
           st.ingested_bytes, st.dropped_bytes, st.pages_written, st.degraded, st.no_storage, st.nor_failed,
           st.verify_failures, st.rs_repairs, st.usable_blocks, st.retired_blocks,
           sd_log_mounted(), sd_log_bytes(), sd_log_dropped(), sd_log_errors(),
           (uint64_t)(esp_timer_get_time() / 1000000));
    cio_printf("CHAIN: tx=%" PRIu32 " rx1=%" PRIu32 "/%" PRIu32 " rx2=%" PRIu32 "/%" PRIu32 " (ok/bad) fwd=%" PRIu32
           " tx_drop=%" PRIu32 " own_dropped=%" PRIu32 " cal_tx=%" PRIu32 " cal_rx=%" PRIu32 "\n",
           cs.tx_packets, cs.rx_ok[0], cs.rx_bad[0], cs.rx_ok[1], cs.rx_bad[1], cs.forwarded, cs.tx_drop, cs.own_dropped,
           cs.tx_cal, cs.rx_cal);
    nau_stats_t ns;
    nau7802_get_stats(&ns);
    cio_printf("LC: running=%d fault=%d samples=%" PRIu32 " i2c_err=%" PRIu32 " backup=%" PRIu32 " restarts=%" PRIu32 " ocal=%06" PRIX32 "\n",
           nau7802_running(), nau7802_fault(), ns.samples, ns.i2c_errors, ns.backup_reads, ns.restarts, ns.ocal);
    cal_reply("CAL:");
    cio_printf("CAL: flash_ok=%d\n", cal_flash_ok());
    uint8_t fl[8], m = chain_seen(SEEN_MS, fl);
    for (int i = 0; i < 8; i++) {                  /* heartbeat flags: 1 rec, 2 auto, 4 sensor, 8 SD, 16 load cell */
        if (m & (1U << i)) { cio_printf("DAQ %d flags=0x%02X\n", i, fl[i]); }
    }
}

/* List the files on the SD card with their sizes. */
static void do_sdls(void)
{
    DIR *d = sd_up() ? opendir("/sdcard") : NULL;
    struct dirent *e;
    if (!d) { cio_printf("SDLS:ERR no card\n"); return; }
    while ((e = readdir(d)) != NULL) {
        struct stat sb;
        char p[300];
        snprintf(p, sizeof p, "/sdcard/%s", e->d_name);
        cio_printf("SDFILE %s %ld\n", e->d_name, stat(p, &sb) == 0 ? (long)sb.st_size : -1L);
    }
    closedir(d);
    cio_printf("SDLS:END\n");
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
    cio_flush();
    cio_write(line, (size_t)len, 50);
}

/* FILES: the PFS files and the SD files, one line each, for the MATLAB file list.
     FILE PFS <file id> <bytes> <flags hex> <start>   start = offset of the first whole record (for CHUNK);
                                                      bytes = from start to the end (approximate if flag 0x0004 is clear)
     FILE SD <name> <bytes> */
static void do_files(void)
{
    uint16_t n = pfs_file_count(s_pfs);
    bool was_on = stream_enabled();
    stream_enable(false);                          /* no number lines inside the file lines */
    cio_printf("FILES:BEGIN\n");
    for (uint16_t i = 0; i < n; i++) {
        pfs_file_info_t fi;
        if (pfs_file_info(s_pfs, i, &fi) == PFS_OK) {
            uint64_t start = (fi.first_offset + sizeof(record_t) - 1) / sizeof(record_t) * sizeof(record_t);
            cio_printf("FILE PFS %u %lu %04X %lu\n", fi.file_id,
                   (unsigned long)(fi.size_bytes > start ? fi.size_bytes - start : 0U), fi.flags, (unsigned long)start);
        }
    }
    DIR *d = sd_up() ? opendir("/sdcard") : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        struct stat sb;
        char p[300];
        snprintf(p, sizeof p, "/sdcard/%s", e->d_name);
        if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) { cio_printf("FILE SD %s %ld\n", e->d_name, (long)sb.st_size); }
    }
    if (d) { closedir(d); }
    cio_printf("FILES:END\n");
    cio_flush();
    if (was_on) { stream_enable(true); }
}

/* ---- CHUNK: one piece of a file, for the download (MATLAB app, tools/daq_get.py) ----
     CHUNK PFS <file id> <offset> <max len>
     CHUNK SD <name> <offset> <max len>          for example CHUNK SD F00012D3.BIN 0 16384
   Reply: the line "CHUNK:<offset> <n> <crc32, 8 hex>", then n raw bytes. n = 0: the end of the file.
   Error: "CHUNK:ERR <reason>". The offset is the file offset; a PFS file starts at the <start> value of FILES.
   crc32 = the zlib CRC-32 of the n bytes. The host asks again for a chunk with a bad CRC, so one bad byte on
   the USB link costs one chunk, not the file. The log is muted while the bytes go out. */
#define CHUNK_MAX 16384U
static uint8_t s_chunk[CHUNK_MAX];

/* The driver puts a write in its TX ring buffer (4096 bytes) in one piece or not at all: a write larger than
   the ring buffer always fails. So write in pieces of USB_PIECE bytes. */
#define USB_PIECE 1024U

static bool usb_put(const uint8_t *p, size_t n)
{
    while (n) {
        size_t k = n < USB_PIECE ? n : USB_PIECE;
        if (cio_write(p, k, 3000) != (int)k) { return false; }   /* the host does not read */
        p += k; n -= k;
    }
    return true;
}

static void do_chunk(const char *src, const char *name, const char *offs, const char *lens)
{
    bool pfs = src && !strcmp(src, "PFS"), sd = src && !strcmp(src, "SD");
    uint32_t off, want;
    size_t n = 0;

    if (!lens || (!pfs && !sd)) { cio_printf("CHUNK:ERR use CHUNK PFS|SD <file> <offset> <len>\n"); return; }
    off = (uint32_t)strtoul(offs, NULL, 10);
    want = (uint32_t)strtoul(lens, NULL, 10);
    if (want > CHUNK_MAX) { want = CHUNK_MAX; }
    if (pfs) {
        uint16_t id = (uint16_t)atoi(name);
        while (n < want) {                       /* pfs_read can give less than asked (a page end) */
            size_t got = 0;
            pfs_err_t e = pfs_read(s_pfs, id, off + n, s_chunk + n, want - n, &got);
            if (e == PFS_ERR_EOF || (e == PFS_OK && got == 0)) { break; }
            if (e != PFS_OK) { cio_printf("CHUNK:ERR pfs_read error %d at offset %lu\n", (int)e, (unsigned long)(off + n)); return; }
            n += got;
        }
    } else {
        char path[48];
        FILE *f;
        if (strchr(name, '/') || strlen(name) > 12) { cio_printf("CHUNK:ERR bad name\n"); return; }
        if (off == 0 && !files_mode_active()) { (void)sd_log_flush(2000); }     /* the first chunk: write the staged bytes first */
        snprintf(path, sizeof path, "/sdcard/%s", name);
        f = sd_up() ? fopen(path, "rb") : NULL;
        if (!f) { cio_printf("CHUNK:ERR no SD file %s\n", name); return; }
        if (fseek(f, (long)off, SEEK_SET) == 0) { n = fread(s_chunk, 1, want, f); }
        bool bad = ferror(f) != 0;
        fclose(f);
        if (bad) { cio_printf("CHUNK:ERR SD read error at offset %lu\n", (unsigned long)off); return; }
    }

    bool was_on = stream_enabled();
    if (was_on) { stream_enable(false); }               /* no number lines inside the chunk */
    esp_log_level_set("*", ESP_LOG_NONE);              /* no log line inside the chunk */
    cio_printf("CHUNK:%lu %u %08lX\n", (unsigned long)off, (unsigned)n,
           (unsigned long)(n ? esp_rom_crc32_le(0, s_chunk, (uint32_t)n) : 0U));
    cio_flush();
    if (n) { (void)usb_put(s_chunk, n); }               /* a failure shows on the host as a short chunk */
    if (was_on) { stream_enable(true); } else {
        esp_log_level_set("*", ESP_LOG_INFO);
#ifdef DAQ_MTP_USB
        usb_files_log_quiet();                       /* REVIEW (MTP-C): the "*" call removed the per-tag WARN levels of the MTP trace tags */
#endif
    }
}

static void do_list(void)
{
    uint16_t n = pfs_file_count(s_pfs);
    cio_printf("LIST:%u files\n", n);
    for (uint16_t i = 0; i < n; i++) {
        pfs_file_info_t fi;
        if (pfs_file_info(s_pfs, i, &fi) == PFS_OK) {
            cio_printf("FILE id=%u flags=0x%04X pages=%" PRIu32 " first_offset=%" PRIu64 " size=%" PRIu64 "\n",
                   fi.file_id, fi.flags, fi.pages, fi.first_offset, fi.size_bytes);
        }
    }
    cio_printf("LIST:END\n");
}

/* The counts of one sensor in CHECK. */
typedef struct {
    uint32_t n, breaks, missing, back;
    uint32_t first_ts, prev_ts, max_gap, near;
    uint64_t near_sum;
    int64_t  sum;
    int32_t  rmin, rmax;
    uint16_t first_seq, prev_seq;
} chk_t;

static void chk_add(chk_t *c, uint32_t ts, int32_t reading, uint16_t seq, uint16_t seq_mask)
{
    if (c->n == 0) { c->first_seq = seq; c->first_ts = ts; c->rmin = INT32_MAX; c->rmax = INT32_MIN; }
    else {
        uint16_t ds = (uint16_t)((seq - c->prev_seq) & seq_mask);
        uint32_t dt = ts - c->prev_ts;
        if (ds != 1) { c->breaks++; c->missing += (uint16_t)((ds - 1U) & seq_mask); }
        if (dt > 0x80000000U) { c->back++; }
        else {
            if (dt > c->max_gap) { c->max_gap = dt; }
            if (dt < 10000U) { c->near++; c->near_sum += dt; }
        }
    }
    if (reading < c->rmin) { c->rmin = reading; }
    if (reading > c->rmax) { c->rmax = reading; }
    c->sum += reading;
    c->prev_seq = seq; c->prev_ts = ts; c->n++;
}

/* A file of the firmware before the load cell (record.h): the top seq bit changes at most one time in a chunk. */
static bool file_has_one_sensor(const uint8_t *buf, size_t got)
{
    unsigned flips = 0;
    int prev = -1;
    for (size_t i = 0; i + sizeof(record_t) <= got; i += sizeof(record_t)) {
        record_t r;
        memcpy(&r, buf + i, sizeof r);
        if (rec_is_meta(&r)) { continue; }          /* a calibration record has no sensor bit */
        int top = (r.seq >> 15) & 1;
        if (prev >= 0 && top != prev) { flips++; }
        prev = top;
    }
    return flips <= 1U;
}

/* Decode records and test each sensor: seq steps by 1, the time goes forward. The interval is about 1.16 ms for the PT
   (860 SPS) and 3.1 ms for the load cell (320 SPS). */
static void do_check(int file_id, uint32_t max_rec)
{
    static uint8_t buf[CHUNK];
    static chk_t pt, lc;
    static cal_parser_t cp;
    cal_t cal_last;
    uint32_t meta = 0, blocks = 0;
    pfs_status_t st;
    pfs_file_info_t fi;
    pfs_get_status(s_pfs, &st);
    if (file_id < 0) { file_id = st.active_file_id; }
    if (pfs_file_stat(s_pfs, (uint16_t)file_id, &fi) != PFS_OK) { cio_printf("CHECK:ERR no file %d\n", file_id); return; }

    uint64_t off = (fi.first_offset + sizeof(record_t) - 1) / sizeof(record_t) * sizeof(record_t);
    uint32_t read_err = 0;
    bool first = true, one_sensor = false;

    memset(&pt, 0, sizeof pt); memset(&lc, 0, sizeof lc); memset(&cp, 0, sizeof cp); memset(&cal_last, 0, sizeof cal_last);
    while (pt.n + lc.n < max_rec) {
        size_t got = 0;
        pfs_err_t e = pfs_read(s_pfs, (uint16_t)file_id, off, buf, CHUNK, &got);
        if (e == PFS_ERR_EOF) { break; }
        if (e != PFS_OK) { read_err++; break; }
        if (got < sizeof(record_t)) { break; }
        if (first) { one_sensor = file_has_one_sensor(buf, got); first = false; }
        for (size_t i = 0; i + sizeof(record_t) <= got && pt.n + lc.n < max_rec; i += sizeof(record_t)) {
            record_t r;
            memcpy(&r, buf + i, sizeof r);
            if (cal_parse(&cp, &r, &cal_last)) { blocks++; }
            if (rec_is_meta(&r)) { meta++; continue; }                  /* a calibration record, not a sample */
            if (!one_sensor && rec_is_lc(&r)) { chk_add(&lc, r.timestamp_us, rec_lc_count(&r), rec_lc_seq(&r), REC_LC_SEQ_MASK); }
            else { chk_add(&pt, r.timestamp_us, r.reading, r.seq, REC_PT_SEQ_MASK); }
        }
        off += got - got % sizeof(record_t);
        vTaskDelay(1);                       /* give the other tasks time */
    }
    cio_printf("CHECK:%s file=%d records=%" PRIu32 " seq_breaks=%" PRIu32 " missing=%" PRIu32 " time_back=%" PRIu32
           " read_errors=%" PRIu32 " seq=%u..%u span_us=%" PRIu32 " mean_interval_us=%" PRIu32 " max_gap_us=%" PRIu32
           " reading_min=%" PRId32 " reading_max=%" PRId32 " reading_mean=%" PRId64 "\n",
           (pt.n && !read_err && !pt.back) ? "OK" : "WARN", file_id, pt.n, pt.breaks, pt.missing, pt.back, read_err,
           pt.first_seq, pt.prev_seq, pt.prev_ts - pt.first_ts, pt.near ? (uint32_t)(pt.near_sum / pt.near) : 0U, pt.max_gap,
           pt.n ? pt.rmin : 0, pt.n ? pt.rmax : 0, pt.n ? pt.sum / pt.n : 0);
    if (lc.n) {
        cio_printf("CHECK:LC records=%" PRIu32 " seq_breaks=%" PRIu32 " missing=%" PRIu32 " time_back=%" PRIu32
               " span_us=%" PRIu32 " mean_interval_us=%" PRIu32 " max_gap_us=%" PRIu32
               " reading_min=%" PRId32 " reading_max=%" PRId32 " reading_mean=%" PRId64 "\n",
               lc.n, lc.breaks, lc.missing, lc.back, lc.prev_ts - lc.first_ts,
               lc.near ? (uint32_t)(lc.near_sum / lc.near) : 0U, lc.max_gap, lc.rmin, lc.rmax, lc.sum / lc.n);
    }
    if (meta) {                                    /* the last whole calibration block of the file */
        cio_printf("CHECK:CAL meta_records=%" PRIu32 " blocks=%" PRIu32 " rev=%" PRIu32 " pt_set=%d pt_a=%.9g pt_b=%.9g"
               " lc_set=%d lc_a=%.9g lc_b=%.9g\n", meta, blocks, blocks ? cal_last.rev : 0U,
               (cal_last.flags & CAL_FLAG_PT) != 0, (double)cal_last.a[CAL_PT], (double)cal_last.b[CAL_PT],
               (cal_last.flags & CAL_FLAG_LC) != 0, (double)cal_last.a[CAL_LC], (double)cal_last.b[CAL_LC]);
    }
}

/* ---- CAL: the calibration values of the sensors (cal.h) ----
     CAL GET | CAL PT <A> <B> | CAL LC <A> <B>
   Reply (one line): "CAL:OK rev=<n> pt_set=<0|1> pt_a=<f> pt_b=<f> lc_set=<0|1> lc_a=<f> lc_b=<f>", or "CAL:ERR <reason>".
   The reply is one USB write, so that it stays whole between the number lines of the live stream. */
static void cal_reply(const char *head)
{
    cal_t c;
    char line[200];
    cal_get(&c);
    int len = snprintf(line, sizeof line, "%s rev=%u pt_set=%d pt_a=%.9g pt_b=%.9g lc_set=%d lc_a=%.9g lc_b=%.9g\r\n", head,
                       (unsigned)c.rev, (c.flags & CAL_FLAG_PT) != 0, (double)c.a[CAL_PT], (double)c.b[CAL_PT],
                       (c.flags & CAL_FLAG_LC) != 0, (double)c.a[CAL_LC], (double)c.b[CAL_LC]);
    cio_flush();
    if (len > 0) { cio_write(line, (size_t)len, 50); }
}

static void do_cal(const char *sensor, const char *as, const char *bs)
{
    int s;
    char *e1, *e2;
    double a, b;

    if (sensor && !strcmp(sensor, "GET")) { cal_reply("CAL:OK"); return; }
    s = (sensor && !strcmp(sensor, "PT")) ? CAL_PT : (sensor && !strcmp(sensor, "LC")) ? CAL_LC : -1;
    if (s < 0 || !as || !bs) { cio_printf("CAL:ERR use CAL GET, CAL PT <A> <B>, or CAL LC <A> <B>\n"); return; }
    a = strtod(as, &e1);
    b = strtod(bs, &e2);
    if (*e1 || *e2 || !isfinite(a) || !isfinite(b) || fabs(a) > 1e30 || fabs(b) > 1e30) { cio_printf("CAL:ERR not a number\n"); return; }
    if (!cal_set(s, (float)a, (float)b, NULL)) { cio_printf("CAL:ERR flash write failed, the values stay until the next reset\n"); return; }
    cal_reply("CAL:OK");
}

/* ---- WIFI and TELEM: the telemetry link (telem.h). The values are saved in the flash and used after the next reboot.
     WIFI GET | WIFI MODE OFF|UPLOAD|DOWNLOAD | WIFI SSID <name> | WIFI PASS <password> | TELEM SERVER <host> <port> <token>
     TELEM DECIM <pt> <lc> | TELEM STATUS | REBOOT
   The SSID and the password are the rest of the line, so they can hold spaces. WIFI PASS with no text = an open network.
   Reply (one line, one USB write): "WIFI:OK mode=<saved> run=<this boot> port=.. host=.. token_set=.. pass_set=.. decim_pt=.. decim_lc=..
   link=.. wifi=.. rssi=.. drops=.. sent=.. reconnects=.. ssid=<name to the end of the line>", or "WIFI:ERR <reason>".
   The password is never sent back. */
static const char *mode_name(telem_mode_t m) { return m == TELEM_UPLOAD ? "UPLOAD" : m == TELEM_DOWNLOAD ? "DOWNLOAD" : "OFF"; }

static void wifi_reply(const char *head)
{
    telem_info_t t;
    char line[320];
    telem_get_info(&t);
    int len = snprintf(line, sizeof line, "%s mode=%s run=%s port=%u host=%s token_set=%d pass_set=%d decim_pt=%u decim_lc=%u link=%d wifi=%d"
                       " rssi=%d drops=%lu sent=%lu reconnects=%lu ssid=%s\r\n", head, mode_name(t.saved_mode), mode_name(t.run_mode),
                       (unsigned)t.port, t.host[0] ? t.host : "-", t.token_set, t.pass_set, (unsigned)t.decim_pt, (unsigned)t.decim_lc,
                       t.link_up, t.wifi_up, t.rssi, (unsigned long)t.drops, (unsigned long)t.sent_frames, (unsigned long)t.reconnects, t.ssid);
    cio_flush();
    if (len > 0) { cio_write(line, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1U, 50); }
}

static char *rest_of_line(void)
{
    char *r = strtok(NULL, "");
    while (r && (*r == ' ' || *r == '\t')) { r++; }
    return r;
}

static void do_wifi(const char *sub)
{
    bool ok = true;
    const char *why = "flash write failed";
    if (!sub || !strcmp(sub, "GET")) { wifi_reply("WIFI:OK"); return; }
    if (!strcmp(sub, "MODE")) {
        char *a = strtok(NULL, " \t");
        telem_mode_t m = (a && !strcmp(a, "OFF")) ? TELEM_OFF : (a && !strcmp(a, "UPLOAD")) ? TELEM_UPLOAD : (a && !strcmp(a, "DOWNLOAD")) ? TELEM_DOWNLOAD : (telem_mode_t)-1;
        if ((int)m < 0) { ok = false; why = "use WIFI MODE OFF, UPLOAD, or DOWNLOAD"; } else { ok = telem_set_mode(m); }
    } else if (!strcmp(sub, "SSID")) {
        char *a = rest_of_line();
        if (!a || !*a || strlen(a) > 32) { ok = false; why = "the SSID has 1 to 32 characters"; } else { ok = telem_set_ssid(a); }
    } else if (!strcmp(sub, "PASS")) {
        char *a = rest_of_line();
        if (a && strlen(a) > 63) { ok = false; why = "the password has 63 characters at most"; } else { ok = telem_set_pass(a ? a : ""); }
    } else { ok = false; why = "use WIFI GET, MODE, SSID, or PASS"; }
    if (ok) { wifi_reply("WIFI:OK"); } else { cio_printf("WIFI:ERR %s\n", why); }
}

static void do_telem(const char *sub)
{
    bool ok = true;
    const char *why = "flash write failed";
    if (!sub || !strcmp(sub, "STATUS")) { wifi_reply("WIFI:OK"); return; }
    if (!strcmp(sub, "SERVER")) {
        char *h = strtok(NULL, " \t"), *p = h ? strtok(NULL, " \t") : NULL, *t = p ? strtok(NULL, " \t") : NULL;
        long port = p ? strtol(p, NULL, 10) : 0;
        if (!h || port < 1 || port > 65535 || (t && strlen(t) > 16) || strlen(h) > 63) { ok = false; why = "use TELEM SERVER <host> <port> <token, 16 characters at most>"; }
        else { ok = telem_set_server(h, (uint16_t)port, t); }
    } else if (!strcmp(sub, "DECIM")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL;
        long pt = a ? strtol(a, NULL, 10) : 0, lc = b ? strtol(b, NULL, 10) : 0;
        if (pt < 1 || pt > 1000 || lc < 1 || lc > 1000) { ok = false; why = "use TELEM DECIM <pt> <lc>, each 1 to 1000 (keep every Nth sample)"; }
        else { ok = telem_set_decim((uint16_t)pt, (uint16_t)lc); }
    } else { ok = false; why = "use TELEM SERVER, DECIM, or STATUS"; }
    if (ok) { wifi_reply("WIFI:OK"); } else { cio_printf("WIFI:ERR %s\n", why); }
}

static void do_dump(int file_id, uint64_t off, size_t len)
{
    uint8_t b[256];
    size_t got = 0;
    if (len > sizeof b) { len = sizeof b; }
    pfs_err_t e = pfs_read(s_pfs, (uint16_t)file_id, off, b, len, &got);
    if (e != PFS_OK && e != PFS_ERR_EOF) { cio_printf("DUMP:ERR %d\n", (int)e); return; }
    cio_printf("DUMP file=%d offset=%" PRIu64 " len=%u ", file_id, off, (unsigned)got);
    for (size_t i = 0; i < got; i++) { cio_printf("%02X", b[i]); }
    cio_printf("\n");
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
    if (rec_is_lc(r)) { return; }                  /* the wave is on the sensor line of the PT */
    if (rec_is_meta(r)) { return; }                /* a calibration record, not a sample */
    if (*have_prev) {
        uint16_t ds = (uint16_t)((r->seq - *pseq) & REC_PT_SEQ_MASK);
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
    cio_printf("  %s: records=%" PRIu32 " seq_breaks=%" PRIu32 " missing=%" PRIu32 " edges=%" PRIu32
           " high_mean=%" PRIu32 " low_mean=%" PRId32 "\n", name, w->n_rec, w->seq_breaks, w->missing, w->edges,
           w->hi_n ? (uint32_t)(w->hi_sum / w->hi_n) : 0U, w->lo_n ? (int32_t)(w->lo_sum / w->lo_n) - 1000 : 0);
}

static void do_compare(int remote)
{
    static wave_t a, b;                               /* a = own data from PFS, b = remote data from SD */
    pfs_status_t st;
    char path[40];
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    if (remote < 0 || remote > 7 || remote == (int)io_addr()) { cio_printf("COMPARE:ERR give the ID of the other board\n"); return; }
    if (!sd_log_flush(3000)) { cio_printf("COMPARE:ERR the SD card does not flush\n"); return; }
    pfs_get_status(s_pfs, &st);
    /* The SD file of the other board: its sensor data, sent over UART, saved under its ID. */
    snprintf(path, sizeof path, "/sdcard/F%05uD%d.BIN", (unsigned)st.active_file_id, remote);
    cio_printf("COMPARE: PFS file %u (own, ID %u) against %s (ID %d, over UART)\n",
           (unsigned)st.active_file_id, io_addr(), path, remote);
    if (!scan_pfs(st.active_file_id, &a)) { cio_printf("COMPARE:ERR no PFS file\n"); return; }
    if (!scan_sd(path, &b)) { cio_printf("COMPARE:ERR no SD file %s\n", path); return; }
    print_wave("PFS ", &a);
    print_wave("SD  ", &b);

    uint32_t n = a.edges < b.edges ? a.edges : b.edges;
    if (n < 4) { cio_printf("COMPARE:FAIL fewer than 4 edges (PFS %" PRIu32 ", SD %" PRIu32 "). Is the wave on the sensor line?\n", a.edges, b.edges); return; }
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
    cio_printf("  edge intervals compared=%" PRIu32 " over_4ms=%" PRIu32 " worst_diff_us=%" PRId32 " edge_shift=%" PRId32
           " span_pfs_us=%" PRIu32 " span_sd_us=%" PRIu32 " high_diff=%" PRId32 " low_diff=%" PRId32 "\n",
           best_cnt - 1, best_bad, worst, (int32_t)best_shift, span_a, span_b, hi_diff, lo_diff);
    cio_printf("COMPARE:%s\n", ok ? "PASS the SD copy of the other board and the PFS data show the same wave" : "FAIL see the values above");
}

/* Files mode (MTP-B-5): a read-only subset. Nothing here starts a recording, a flush, or a write to NVS. Returns true if the
   command is done. false = the normal dispatch runs it (PING, LED, LIST, FILES, SDLS, CHUNK, CHECK, DUMP). */
static bool files_mode_cmd(const char *tok)
{
    /* REVIEW (MTP-A): the decision is files_mode_classify() (files_mode_cmd.h, host test tools/mtp_hosttest/cmdtest.c). */
    fmc_t c = files_mode_classify(tok, files_mode_cmd_wants_arg(tok) ? strtok(NULL, " \t") : NULL);
    if (c == FMC_NORMAL) { return false; }
    if (c == FMC_STATUS) {
        pfs_status_t st;
        pfs_get_status(s_pfs, &st);
        cio_printf("STATUS:OK mode=files rec=0 mounted=%d index=%d blocked_writes=%lu nor_blocked=%lu lost_pages=%lu\n",
                       st.mounted, st.index_ready, (unsigned long)mtp_pfs_blocked_writes(),
                       (unsigned long)mtp_pfs_blocked_nor_writes(), (unsigned long)mtp_store_pfs_lost_pages());
    } else if (c == FMC_MATLAB_PING) {            /* REVIEW (MTP-A): the MATLAB apps connect with this. The banner only: no stream_enable(), no task. */
        cio_printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n");
    } else if (c == FMC_STREAM_OFF) {             /* REVIEW (MTP-A): the apps wait for this reply before a file list or a download. No stream runs. */
        cio_printf("STREAM:OFF\n");
    } else if (c == FMC_REBOOT) {                 /* restarts in files mode again (no flush: nothing is open) */
        cio_printf("REBOOT:OK\n");
        cio_flush();
#ifdef DAQ_MTP
        files_mode_restart(FM_REBOOT);             /* drops D+ first (USB stack), then restarts */
#else
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
#endif
#ifdef DAQ_MTP
    } else if (c == FMC_DL) {                     /* ROM download mode: esptool works with no BOOT button (Spike_Report.md MTP-P-9.2) */
        cio_printf("DL:OK rebooting to ROM download mode\n");
        cio_flush();
        files_mode_restart(FM_DOWNLOAD);
#endif
    } else if (c == FMC_LEAVE) {                  /* restarts in serial mode once, with MODE2 still low */
        cio_printf("LEAVE:OK\n");
        cio_flush();
        vTaskDelay(pdMS_TO_TICKS(200));
        files_mode_leave();
    } else {
        cio_printf("ERR not available in files mode (read-only)\n");
    }
    return true;
}

static void run(char *line)
{
    char *tok = strtok(line, " \t");
    if (!tok) { return; }
    if (files_mode_active() && files_mode_cmd(tok)) { cio_flush(); return; }
    if (!strcmp(tok, "PING")) { cio_printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n"); }
    else if (!strcmp(tok, "MATLAB")) {            /* MATLAB PING: banner, then one voltage per line */
        char *a = strtok(NULL, " \t");
        if (a && !strcmp(a, "PING")) { cio_printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n"); cio_flush(); stream_enable(true); }
    }
    else if (!strcmp(tok, "LED")) {               /* LED <0..255>: brightness of all LEDs */
        char *a = strtok(NULL, " \t");
        if (a) { long v = strtol(a, NULL, 10); led_set_brightness((uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v)); }
        cio_printf("LED:%u\n", led_get_brightness());
    }
    else if (!strcmp(tok, "STREAM")) {             /* STREAM ON | STREAM OFF */
        char *a = strtok(NULL, " \t");
        stream_enable(a && !strcmp(a, "ON"));
        if (!stream_enabled()) { cio_printf("STREAM:OFF\n"); }
    }
#ifdef DAQ_PROFILE
    else if (!strcmp(tok, "PROFILE")) { prof_print(); }
    else if (!strcmp(tok, "REBOOT")) { char *a = strtok(NULL, " \t"); prof_reboot(a && !strcmp(a, "WRAP")); }
#else
    else if (!strcmp(tok, "REBOOT")) {            /* a new WiFi mode starts after it. Not while recording: a reset would lose the buffered data. */
        if (rec_get()) { cio_printf("REBOOT:ERR recording is on, send REC OFF first\n"); }
        else {
            (void)pfs_flush(s_pfs, 2000);
            (void)sd_log_flush(2000);
            cio_printf("REBOOT:OK\n");
            cio_flush();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
#endif
    else if (!strcmp(tok, "WIFI")) { do_wifi(strtok(NULL, " \t")); }
    else if (!strcmp(tok, "TELEM")) { do_telem(strtok(NULL, " \t")); }
    else if (!strcmp(tok, "DAQS")) { do_daqs(); }
    else if (!strcmp(tok, "FILES")) { do_files(); }
    else if (!strcmp(tok, "CHUNK")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL;
        char *c = b ? strtok(NULL, " \t") : NULL, *d = c ? strtok(NULL, " \t") : NULL;
        do_chunk(a, b, c, d);
    }
    else if (!strcmp(tok, "STATUS")) { cio_printf("STATUS:OK (SD Overflows: %lu)\n", (unsigned long)sd_log_dropped()); }   /* as the sketch */
    else if (!strcmp(tok, "INFO")) { do_info(); }
    else if (!strcmp(tok, "LIST")) { do_list(); }
    else if (!strcmp(tok, "SDLS")) { do_sdls(); }
    else if (!strcmp(tok, "COMPARE")) { char *a = strtok(NULL, " \t"); do_compare(a ? atoi(a) : -1); }
    else if (!strcmp(tok, "REC")) {
        char *a = strtok(NULL, " \t");
        if (a && !strcmp(a, "ON") && rec_locked()) {          /* daq_mtp build: no recording at all (Docs/MTP/Boot_Integration.md MTP-B-4) */
            cio_printf("REC:ERR recording is disabled in this build (daq_mtp)\n");
        } else {
            if (a && !strcmp(a, "ON")) { rec_set(true); } else if (a && !strcmp(a, "OFF")) { rec_set(false); }
            cio_printf("REC:%s\n", rec_get() ? "ON" : "OFF");
        }
    }
    else if (!strcmp(tok, "FLUSH")) { cio_printf(sd_log_flush(2000) ? "SD_FLUSHED:OK\n" : "SD_FLUSHED:ERR\n"); }   /* as the sketch */
    else if (!strcmp(tok, "PFSFLUSH")) {
        bool ok = pfs_flush(s_pfs, 2000) == PFS_OK;
        if (ok) { cal_request(); }                 /* the next PFS file starts with a calibration block */
        cio_printf("PFSFLUSH:%s\n", ok ? "OK" : "ERR");
    }
    else if (!strcmp(tok, "CAL")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL, *c = b ? strtok(NULL, " \t") : NULL;
        do_cal(a, b, c);
    }
    else if (!strcmp(tok, "CHECK")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL;
        do_check(a ? atoi(a) : -1, b ? (uint32_t)strtoul(b, NULL, 10) : 200000U);
    }
    else if (!strcmp(tok, "DUMP")) {
        char *a = strtok(NULL, " \t"), *b = a ? strtok(NULL, " \t") : NULL, *c = b ? strtok(NULL, " \t") : NULL;
        if (c) { do_dump(atoi(a), strtoull(b, NULL, 10), (size_t)strtoul(c, NULL, 10)); } else { cio_printf("DUMP:ERR args\n"); }
    }
    else { cio_printf("ERR unknown command\n"); }
    cio_flush();
}

static void cmd_task(void *arg)
{
    char line[160];                                 /* WIFI PASS holds up to 63 characters */
    size_t n = 0;
    (void)arg;
    for (;;) {
        uint8_t c;
        if (cio_read(&c, 1, IDLE_MS) != 1) {
            if (n) { line[n] = 0; n = 0; run(line); }   /* MATLAB writes "MATLAB PING" without a line end */
            continue;
        }
        if (c == '\n' || c == '\r') { line[n] = 0; n = 0; run(line); }
        else if (n < sizeof line - 1) { line[n++] = (char)c; }
    }
}

void cmd_start(pfs_ctx_t *pfs)
{
    s_pfs = pfs;
    cio_init();                                /* serial mode: the USB Serial/JTAG driver; files mode: the stub backend */
    xTaskCreatePinnedToCore(cmd_task, "cmd", 8192, NULL, 1, NULL, 0);
}
