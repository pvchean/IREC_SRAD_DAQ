/* IREC SRAD DAQ: ADS1115 (860 SPS) -> ISR -> task -> pfs_ingest() -> Paranoid FS on the ZD35Q1GC.
   The start-up follows firmware/main_esp32s3.c of Paranoid-FS (HAL-V3-7.6). */
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pfs.h"
#include "pfs_esp32s3.h"
#include "pfs_zd35q1gc.h"

#include "ads1115.h"
#include "board.h"

#define WRITER_STACK 8192U
#define SCAN_STACK   4096U

static const char *TAG = "daq";

/* The record on NAND: 8 bytes, little endian. The seq field shows lost samples. */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_us;
    int16_t  reading;
    uint16_t seq;
} record_t;

static pfs_ctx_t   *s_pfs;
static StaticTask_t s_writer_tcb, s_scan_tcb;
static StackType_t  s_writer_stack[WRITER_STACK], s_scan_stack[SCAN_STACK];
static uint16_t     s_seq;

static void on_sample(const ads_sample_t *s)
{
    record_t r = { s->timestamp_us, s->reading, s_seq++ };
    (void)pfs_ingest(s_pfs, (const uint8_t *)&r, sizeof r);   /* never waits; the status counts drops */
}

static void writer_task(void *a) { pfs_writer_run((pfs_ctx_t *)a); }
static void scan_task(void *a)   { pfs_scan_run((pfs_ctx_t *)a); vTaskDelete(NULL); }

void app_main(void)
{
    static const pfs_config_t cfg = { .device_id = 1U, .writer_timeout_ms = 1000U, .max_ingest_packet = 1024U };
    const pfs_os_hal_t *os = pfs_os_esp32s3_get();
    spi_bus_esp32s3_cfg_t bus_cfg = {
        .mosi = NAND_MOSI_PIN, .miso = NAND_MISO_PIN, .sclk = NAND_CLK_PIN, .cs = NAND_CS_PIN,
        .wp = NAND_WP_PIN, .hold = NAND_HOLD_PIN, .clock_hz = 40000000U, .log = os->log
    };
    static const pfs_nor_littlefs_cfg_t nor_cfg = { .partition_label = "pfs_nor", .base_path = "/pfs" };

    if (pfs_early_init(&s_pfs, os, &cfg) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_early_init failed");
        return;
    }
    const spi_bus_hal_t *bus  = spi_bus_esp32s3_get(&bus_cfg);
    const nand_hal_t    *nand = nand_zd35q1gc_bind(bus);
    const pfs_nor_hal_t *nor  = pfs_nor_littlefs_get(&nor_cfg);

    /* The sensor starts first: pfs_ingest() buffers the data during the mount (HL-V3-9.1). */
    if (ads1115_start(on_sample) != 0) { ESP_LOGE(TAG, "sensor not started"); }

    while (pfs_mount(s_pfs, nand, nor) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_mount failed, try again in 1 s");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    (void)xTaskCreateStaticPinnedToCore(writer_task, "pfs_writer", WRITER_STACK, s_pfs, 5,
                                        s_writer_stack, &s_writer_tcb, 1);
    (void)xTaskCreateStaticPinnedToCore(scan_task, "pfs_scan", SCAN_STACK, s_pfs, 2,
                                        s_scan_stack, &s_scan_tcb, 1);

    for (;;) {
        pfs_status_t st;
        vTaskDelay(pdMS_TO_TICKS(1000));
        pfs_get_status(s_pfs, &st);
        ESP_LOGI(TAG, "in=%llu drop=%llu pages=%lu degraded=%d nor_fail=%d seq=%u",
                 (unsigned long long)st.ingested_bytes, (unsigned long long)st.dropped_bytes,
                 (unsigned long)st.pages_written, st.degraded, st.nor_failed, s_seq);
    }
}
