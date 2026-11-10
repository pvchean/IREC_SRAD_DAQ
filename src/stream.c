#include "stream.h"

#include <stdarg.h>
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "cal.h"
#include "telem.h"

#define SB_SIZE    4096U
#define BATCH      32U                    /* records per USB write */
#define VOLT_PER_LSB 0.0001875f           /* +/-6.144 V range */

static StreamBufferHandle_t s_sb;
static StaticStreamBuffer_t s_sb_struct;
static uint8_t s_sb_store[SB_SIZE + 1];
static volatile bool s_on;
static volatile bool s_busy;               /* the task is in a USB write */

bool stream_enabled(void) { return s_on; }

void stream_enable(bool on)
{
    if (on && !s_on) {
        xStreamBufferReset(s_sb);
        esp_log_level_set("*", ESP_LOG_NONE);        /* log lines would break the number lines */
    } else if (!on && s_on) {
        esp_log_level_set("*", ESP_LOG_INFO);
    }
    s_on = on;
    while (!on && s_busy) { vTaskDelay(1); }       /* after the return, no more number lines come */
}

void stream_push(const record_t *r)
{
    /* A full buffer drops the sample. A part of a record would move the grid of the reader. */
    if (s_on && xStreamBufferSpacesAvailable(s_sb) >= sizeof *r) { (void)xStreamBufferSend(s_sb, r, sizeof *r, 0); }
}

/* The DOWNLOAD DAQ (telem.c) may wait: it is not a sensor task. It is the only writer in that mode (the sink does not call
   stream_push there). It waits for room, so a burst of records from the server is not lost. */
bool stream_push_wait(const record_t *r, uint32_t timeout_ms)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_on) {
        if (xStreamBufferSpacesAvailable(s_sb) >= sizeof *r) { return xStreamBufferSend(s_sb, r, sizeof *r, 0) == sizeof *r; }
        if ((int32_t)(xTaskGetTickCount() - end) >= 0) { return false; }
        vTaskDelay(1);
    }
    return false;
}

/* Add one line to the text of a batch. The line is cut if it does not fit (a text line is never longer than 160 bytes). */
static size_t add_line(char *line, size_t size, size_t len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t add_line(char *line, size_t size, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + len, size - len, fmt, ap);
    va_end(ap);
    return (n > 0 && (size_t)n < size - len) ? len + (size_t)n : len;
}

static void stream_task(void *arg)
{
    record_t r[BATCH];
    char line[BATCH * 12 + 512];
    cal_parser_t cp = { 0 };                       /* DOWNLOAD DAQ: the calibration block of the upload DAQ comes as 6 META records */
    (void)arg;
    for (;;) {
        size_t n = xStreamBufferReceive(s_sb, r, sizeof r, pdMS_TO_TICKS(50)) / sizeof r[0];
        size_t len = 0;
        s_busy = true;                              /* set before the s_on test: see stream_enable() */
        if (!s_on) { s_busy = false; continue; }
        for (size_t i = 0; i < n; i++) {
            if (rec_is_meta(&r[i])) {
                uint32_t kind = r[i].timestamp_us - REC_META_BASE;
                cal_t c;
                if (kind == TELEM_META_DTPT || kind == TELEM_META_DTLC) {
                    len = add_line(line, sizeof line, len, kind == TELEM_META_DTPT ? "T:DTPT %lu\n" : "T:DTLC %lu\n", (unsigned long)rec_meta_payload(&r[i]));
                } else if (kind == TELEM_META_AGE) { len = add_line(line, sizeof line, len, "T:AGE %ld\n", (long)(int32_t)rec_meta_payload(&r[i])); }
                else if (kind == TELEM_META_LINK) {
                    uint32_t p = rec_meta_payload(&r[i]);
                    len = add_line(line, sizeof line, len, "T:LINK %u %d %d\n", (unsigned)(p & 0xFFFFU), (int)(int8_t)(p >> 16), (int)(int8_t)(p >> 24));
                } else if (cal_parse(&cp, &r[i], &c)) {       /* a whole block with a good CRC */
                    len = add_line(line, sizeof line, len, "CALRX:OK rev=%u pt_set=%d pt_a=%.9g pt_b=%.9g lc_set=%d lc_a=%.9g lc_b=%.9g\n",
                                   (unsigned)c.rev, (c.flags & CAL_FLAG_PT) != 0, (double)c.a[CAL_PT], (double)c.b[CAL_PT],
                                   (c.flags & CAL_FLAG_LC) != 0, (double)c.a[CAL_LC], (double)c.b[CAL_LC]);
                }
                continue;
            }
            if (rec_is_lc(&r[i])) { len += (size_t)snprintf(line + len, sizeof line - len, "L:%ld\n", (long)rec_lc_count(&r[i])); }
            else { len += (size_t)snprintf(line + len, sizeof line - len, "%.3f\n", (double)r[i].reading * VOLT_PER_LSB); }
        }
        /* A short timeout: with no host reading, the sample task never waits for this write. */
        if (len) { usb_serial_jtag_write_bytes(line, len, pdMS_TO_TICKS(20)); }
        s_busy = false;
    }
}

void stream_start(void)
{
    s_sb = xStreamBufferCreateStatic(SB_SIZE, sizeof(record_t), s_sb_store, &s_sb_struct);
    xTaskCreatePinnedToCore(stream_task, "stream", 4096, NULL, 1, NULL, 0);
}
