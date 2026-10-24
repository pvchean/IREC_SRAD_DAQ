#include "stream.h"

#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

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
    if (s_on) { (void)xStreamBufferSend(s_sb, r, sizeof *r, 0); }   /* a full buffer drops the sample */
}

static void stream_task(void *arg)
{
    record_t r[BATCH];
    char line[BATCH * 12];
    (void)arg;
    for (;;) {
        size_t n = xStreamBufferReceive(s_sb, r, sizeof r, pdMS_TO_TICKS(50)) / sizeof r[0];
        size_t len = 0;
        s_busy = true;                              /* set before the s_on test: see stream_enable() */
        if (!s_on) { s_busy = false; continue; }
        for (size_t i = 0; i < n; i++) {
            len += (size_t)snprintf(line + len, sizeof line - len, "%.3f\n", (double)r[i].reading * VOLT_PER_LSB);
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
