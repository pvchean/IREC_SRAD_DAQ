#include "cmd.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io.h"
#include "record.h"
#include "sd_log.h"
#include "stream.h"

#define CHUNK 1024U                      /* multiple of sizeof(record_t) */

static pfs_ctx_t *s_pfs;

static void do_status(void)
{
    pfs_status_t st;
    pfs_get_status(s_pfs, &st);
    printf("STATUS:OK rec=%d auto=%d addr=%u mounted=%d path=%u index=%d file=%u ingested=%" PRIu64 " dropped=%" PRIu64
           " pages=%" PRIu32 " degraded=%d no_storage=%d nor_failed=%d verify_fail=%" PRIu32 " rs_repairs=%" PRIu32
           " usable_blocks=%u retired=%u sd=%d sd_bytes=%" PRIu64 " sd_drop=%" PRIu32 " sd_err=%" PRIu32 " up_s=%" PRIu64 "\n",
           rec_get(), io_auto_mode(), io_addr(), st.mounted, st.mount_path, st.index_ready, st.active_file_id,
           st.ingested_bytes, st.dropped_bytes, st.pages_written, st.degraded, st.no_storage, st.nor_failed,
           st.verify_failures, st.rs_repairs, st.usable_blocks, st.retired_blocks,
           sd_log_mounted(), sd_log_bytes(), sd_log_dropped(), sd_log_errors(),
           (uint64_t)(esp_timer_get_time() / 1000000));
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

static void run(char *line)
{
    char *tok = strtok(line, " \t");
    if (!tok) { return; }
    if (!strcmp(tok, "PING")) { printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n"); }
    else if (!strcmp(tok, "MATLAB")) {            /* MATLAB PING: banner, then one voltage per line */
        char *a = strtok(NULL, " \t");
        if (a && !strcmp(a, "PING")) { printf("01010101 10101010 DAQBOARD V1 01010101 10101010\n"); fflush(stdout); stream_enable(true); }
    }
    else if (!strcmp(tok, "STREAM")) { stream_enable(false); printf("STREAM:OFF\n"); }
    else if (!strcmp(tok, "STATUS")) { do_status(); }
    else if (!strcmp(tok, "LIST")) { do_list(); }
    else if (!strcmp(tok, "REC")) {
        char *a = strtok(NULL, " \t");
        if (a && !strcmp(a, "ON")) { rec_set(true); } else if (a && !strcmp(a, "OFF")) { rec_set(false); }
        printf("REC:%s\n", rec_get() ? "ON" : "OFF");
    }
    else if (!strcmp(tok, "FLUSH")) { printf("FLUSH:%s\n", pfs_flush(s_pfs, 2000) == PFS_OK ? "OK" : "ERR"); }
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
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100)) != 1) { continue; }
        if (c == '\n' || c == '\r') { line[n] = 0; n = 0; run(line); }
        else if (n < sizeof line - 1) { line[n++] = (char)c; }
    }
}

void cmd_start(pfs_ctx_t *pfs)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    s_pfs = pfs;
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(cmd_task, "cmd", 6144, NULL, 1, NULL, 0);
}
