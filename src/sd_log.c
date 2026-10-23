#include "sd_log.h"

#include <stdio.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "board.h"

#define CHUNK       4096U                 /* multiple of 512: good for SDMMC DMA */
#define SB_SIZE     16384U                /* about 2.3 s of data at 860 SPS; internal RAM */

static const char *TAG = "sd";
static pfs_ctx_t *s_pfs;
static StreamBufferHandle_t s_sb;
static StaticStreamBuffer_t s_sb_struct;
static uint8_t s_sb_store[SB_SIZE + 1];
static uint8_t s_chunk[CHUNK] __attribute__((aligned(4)));
static volatile bool s_mounted;
static volatile uint32_t s_dropped, s_errors;
static volatile uint64_t s_bytes;

bool sd_log_mounted(void)    { return s_mounted; }
uint32_t sd_log_dropped(void) { return s_dropped; }
uint32_t sd_log_errors(void)  { return s_errors; }
uint64_t sd_log_bytes(void)   { return s_bytes; }

void sd_log_push(const record_t *r)
{
    if (s_sb && xStreamBufferSend(s_sb, r, sizeof *r, 0) != sizeof *r) { s_dropped++; }
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
    mc.max_files = 2;
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card) == ESP_OK;
}

static void sd_task(void *arg)
{
    FILE *f = NULL;
    (void)arg;
    for (;;) {
        if (!s_mounted) {
            if (mount_card()) { s_mounted = true; ESP_LOGI(TAG, "card mounted"); }
            else { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        }
        size_t n = xStreamBufferReceive(s_sb, s_chunk, CHUNK, pdMS_TO_TICKS(1000));
        if (n == 0) {                                   /* idle (not recording): close the file */
            if (f) { fclose(f); f = NULL; }
            continue;
        }
        if (!f) {
            pfs_status_t st;
            char name[32];
            pfs_get_status(s_pfs, &st);
            snprintf(name, sizeof name, "/sdcard/F%05u.BIN", (unsigned)st.active_file_id);
            f = fopen(name, "ab");
            if (f) { ESP_LOGI(TAG, "file %s", name); }
        }
        if (!f || fwrite(s_chunk, 1, n, f) != n || fflush(f) != 0 || fsync(fileno(f)) != 0) {
            s_errors++;
            if (f) { fclose(f); f = NULL; }
            esp_vfs_fat_sdcard_unmount("/sdcard", NULL);   /* the card may be out: mount again */
            s_mounted = false;
        } else {
            s_bytes += n;
        }
    }
}

void sd_log_start(pfs_ctx_t *pfs)
{
    s_pfs = pfs;
    s_sb = xStreamBufferCreateStatic(SB_SIZE, 1, s_sb_store, &s_sb_struct);
    xTaskCreatePinnedToCore(sd_task, "sd", 6144, NULL, 2, NULL, 0);
}
