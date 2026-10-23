#include "sd_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "board.h"
#include "io.h"

#define CHUNK       4096U                 /* multiple of 512: good for SDMMC DMA */
#define SB_SIZE     16384U                /* about 2.3 s of data at 860 SPS; internal RAM */
#define REM_SIZE    8192U                 /* records of the other boards, waiting for the SD task */
#define NDEV        8
#define PARTIAL_MS  500                   /* write a partial chunk after this idle time (save often) */
#define CLOSE_MS    1500                  /* close the file after this idle time */

static const char *TAG = "sd";
static pfs_ctx_t *s_pfs;
static StreamBufferHandle_t s_sb;
static StaticStreamBuffer_t s_sb_struct;
static uint8_t s_sb_store[SB_SIZE + 1];
static MessageBufferHandle_t s_rem;
static StaticMessageBuffer_t s_rem_struct;
static uint8_t s_rem_store[REM_SIZE + 1];
static volatile bool s_mounted;
static volatile uint32_t s_dropped, s_errors;
static volatile uint64_t s_bytes;

/* One file for each device ID: the own ID and the IDs of the other boards. */
typedef struct { FILE *f; uint8_t *buf; size_t n; TickType_t last; } dev_t_;
static dev_t_ s_dev[NDEV];

bool sd_log_mounted(void)    { return s_mounted; }
uint32_t sd_log_dropped(void) { return s_dropped; }
uint32_t sd_log_errors(void)  { return s_errors; }
uint64_t sd_log_bytes(void)   { return s_bytes; }

void sd_log_push(const record_t *r)
{
    if (s_sb && xStreamBufferSend(s_sb, r, sizeof *r, 0) != sizeof *r) { s_dropped++; }
}

void sd_log_push_remote(uint8_t dev, const uint8_t *records, size_t len)
{
    uint8_t m[1 + 32 * 8];
    if (!s_rem || len > sizeof m - 1) { return; }
    m[0] = dev;
    memcpy(m + 1, records, len);
    if (xMessageBufferSend(s_rem, m, len + 1, 0) != len + 1) { s_dropped++; }
}

static bool mount_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_mount_config_t mc = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    sdmmc_card_t *card;

    slot.width = 1;
    slot.clk = SDMMC_CLK_PIN;
    slot.cmd = SDMMC_CMD_PIN;
    slot.d0  = SDMMC_DAT0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    mc.max_files = 9;
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card) == ESP_OK;
}

/* Write the staged bytes of one device. The name uses the PFS file ID at the time the file opens. */
static bool flush_dev(int id)
{
    dev_t_ *d = &s_dev[id];
    if (d->n == 0) { return true; }
    if (!d->f) {
        pfs_status_t st;
        char name[32];
        pfs_get_status(s_pfs, &st);
        snprintf(name, sizeof name, "/sdcard/F%05uD%d.BIN", (unsigned)st.active_file_id, id);
        d->f = fopen(name, "ab");
        if (d->f) { ESP_LOGI(TAG, "file %s", name); }
    }
    bool ok = d->f && fwrite(d->buf, 1, d->n, d->f) == d->n && fflush(d->f) == 0 && fsync(fileno(d->f)) == 0;
    if (ok) { s_bytes += d->n; }
    d->n = 0;
    return ok;
}

static bool write_dev(int id, const uint8_t *data, size_t len)
{
    dev_t_ *d = &s_dev[id];
    if (!d->buf) { d->buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }
    if (!d->buf) { s_dropped++; return true; }
    d->last = xTaskGetTickCount();
    while (len) {
        size_t k = CHUNK - d->n < len ? CHUNK - d->n : len;
        memcpy(d->buf + d->n, data, k);
        d->n += k; data += k; len -= k;
        if (d->n == CHUNK && !flush_dev(id)) { return false; }
    }
    return true;
}

static void sd_fail(void)
{
    s_errors++;
    for (int i = 0; i < NDEV; i++) {
        if (s_dev[i].f) { fclose(s_dev[i].f); s_dev[i].f = NULL; }
        s_dev[i].n = 0;
    }
    esp_vfs_fat_sdcard_unmount("/sdcard", NULL);       /* the card may be out: mount again */
    s_mounted = false;
}

static void sd_task(void *arg)
{
    uint8_t tmp[512], rm[1 + 32 * 8];
    (void)arg;
    for (;;) {
        bool ok = true;
        if (!s_mounted) {
            if (mount_card()) { s_mounted = true; ESP_LOGI(TAG, "card mounted"); }
            else { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        }
        size_t n = xStreamBufferReceive(s_sb, tmp, sizeof tmp, pdMS_TO_TICKS(20));
        if (n) { ok = write_dev(io_addr(), tmp, n); }
        size_t m;
        while (ok && (m = xMessageBufferReceive(s_rem, rm, sizeof rm, 0)) > 1) { ok = write_dev(rm[0] & (NDEV - 1), rm + 1, m - 1); }
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; ok && i < NDEV; i++) {
            dev_t_ *d = &s_dev[i];
            if (d->n && now - d->last >= pdMS_TO_TICKS(PARTIAL_MS)) { ok = flush_dev(i); }
            if (ok && d->f && !d->n && now - d->last >= pdMS_TO_TICKS(CLOSE_MS)) { fclose(d->f); d->f = NULL; }
        }
        if (!ok) { sd_fail(); }
    }
}

void sd_log_start(pfs_ctx_t *pfs)
{
    s_pfs = pfs;
    s_rem = xMessageBufferCreateStatic(REM_SIZE, s_rem_store, &s_rem_struct);
    s_sb = xStreamBufferCreateStatic(SB_SIZE, 1, s_sb_store, &s_sb_struct);
    xTaskCreatePinnedToCore(sd_task, "sd", 6144, NULL, 2, NULL, 0);
}
