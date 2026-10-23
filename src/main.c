/* IREC SRAD DAQ: ADS1115 (860 SPS) -> ISR -> task -> pfs_ingest() -> Paranoid FS on the ZD35Q1GC.
   The start-up follows firmware/main_esp32s3.c of Paranoid-FS (HAL-V3-7.6). */
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pfs.h"
#include "pfs_esp32s3.h"
#include "pfs_zd35q1gc.h"

#include "ads1115.h"
#include "board.h"
#include "cmd.h"
#include "io.h"
#include "led_sr.h"
#include "record.h"
#include "sd_log.h"
#include "stream.h"

#define WRITER_STACK 8192U
#define SCAN_STACK   4096U

static const char *TAG = "daq";


static pfs_ctx_t   *s_pfs;
static StaticTask_t s_writer_tcb, s_scan_tcb;
static StackType_t  s_writer_stack[WRITER_STACK], s_scan_stack[SCAN_STACK];
static uint16_t     s_seq;

static void on_sample(const ads_sample_t *s)
{
    record_t r = { s->timestamp_us, s->reading, s_seq++ };
    stream_push(&r);                                           /* live readings, only after MATLAB PING */
    if (!rec_get()) { return; }                                /* not recording: the sample is not stored */
    (void)pfs_ingest(s_pfs, (const uint8_t *)&r, sizeof r);   /* never waits; the status counts drops */
    sd_log_push(&r);                                           /* the backup copy */
}

#ifdef DAQ_SIM
/* Test build: a sawtooth in place of the ADS1115, 1 kHz. It uses the same path (on_sample -> pfs_ingest). */
static void sim_task(void *a)
{
    (void)a;
    TickType_t t = xTaskGetTickCount();
    for (uint32_t i = 0;; i++) {
        ads_sample_t s = { (uint32_t)esp_timer_get_time(), (int16_t)(i & 0x7FFF) };
        on_sample(&s);
        vTaskDelayUntil(&t, 1);
    }
}

/* Read the active file back and check that the seq field counts up with no gap. */
static void readback_check(void)
{
    pfs_status_t st;
    pfs_file_info_t fi;
    uint8_t buf[512];
    size_t got = 0;
    uint16_t expect = 0;
    uint32_t checked = 0, gaps = 0;

    pfs_get_status(s_pfs, &st);
    if (pfs_file_stat(s_pfs, st.active_file_id, &fi) != PFS_OK) { ESP_LOGE(TAG, "READBACK: no file"); return; }
    for (uint64_t off = 0; off + sizeof buf <= fi.size_bytes && checked < 2000; off += sizeof buf) {
        if (pfs_read(s_pfs, st.active_file_id, off, buf, sizeof buf, &got) != PFS_OK || got != sizeof buf) { break; }
        for (size_t i = 0; i + sizeof(record_t) <= got; i += sizeof(record_t)) {
            record_t r;
            memcpy(&r, buf + i, sizeof r);
            if (off == 0 && i == 0) { expect = r.seq; }
            if (r.seq != expect || (uint16_t)r.reading != (uint16_t)(r.seq & 0x7FFF)) { gaps++; expect = r.seq; }
            expect++; checked++;
        }
        /* a 512-byte block holds 64 records: the record grid stays aligned */
    }
    ESP_LOGI(TAG, "READBACK: file %u size=%llu records_checked=%lu errors=%lu",
             (unsigned)st.active_file_id, (unsigned long long)fi.size_bytes, (unsigned long)checked, (unsigned long)gaps);
}
#endif

/* The LEDs show the state, 20 times per second. The LED of the sketch (Working) blinks. */
static void led_task(void *a)
{
    (void)a;
    for (uint32_t n = 0;; n++) {
        pfs_status_t st = { 0 };
        if (s_pfs) { pfs_get_status(s_pfs, &st); }
        bool sensor = ads1115_running();
        bool err = !s_pfs || st.degraded || st.no_storage || st.nor_failed || st.dropped_bytes > 0 ||
                   (!sensor && n > 60);
        led_set(LED_WORKING, (n / 8) % 2 == 0);
        led_set(LED_RECORDING, rec_get());
        led_set(LED_AUTOREC, io_auto_mode());
        led_set(LED_WIFI, false);
        led_set(LED_NAND, st.mounted && !st.degraded && !st.no_storage && st.verify_failures == 0);
        led_set(LED_SD, sd_log_mounted());
        led_set(LED_ERROR, err);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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

    led_sr_init();
    io_init();                                 /* the level of the mode pins sets recording at power-up */
    xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 1, NULL, 0);

    if (pfs_early_init(&s_pfs, os, &cfg) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_early_init failed");
        return;
    }
    const spi_bus_hal_t *bus  = spi_bus_esp32s3_get(&bus_cfg);
    const nand_hal_t    *nand = nand_zd35q1gc_bind(bus);
    const pfs_nor_hal_t *nor  = pfs_nor_littlefs_get(&nor_cfg);

    /* The sensor starts first: pfs_ingest() buffers the data during the mount (HL-V3-9.1). */
#ifdef DAQ_SIM
    xTaskCreatePinnedToCore((TaskFunction_t)sim_task, "sim", 4096, NULL, 6, NULL, 1);
#else
    if (ads1115_start(on_sample) != 0) { ESP_LOGE(TAG, "sensor not started"); }
#endif

    while (pfs_mount(s_pfs, nand, nor) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_mount failed, try again in 1 s");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    (void)xTaskCreateStaticPinnedToCore(writer_task, "pfs_writer", WRITER_STACK, s_pfs, 5,
                                        s_writer_stack, &s_writer_tcb, 1);
    (void)xTaskCreateStaticPinnedToCore(scan_task, "pfs_scan", SCAN_STACK, s_pfs, 2,
                                        s_scan_stack, &s_scan_tcb, 1);

    stream_start();
    sd_log_start(s_pfs);
    cmd_start(s_pfs);

    for (int n = 0;; n++) {
        pfs_status_t st;
        vTaskDelay(pdMS_TO_TICKS(1000));
#ifdef DAQ_SIM
        if (n == 10) { readback_check(); }
#else
        (void)n;
#endif
        pfs_get_status(s_pfs, &st);
        if (n % 10 == 0) ESP_LOGI(TAG, "in=%llu drop=%llu pages=%lu degraded=%d nor_fail=%d seq=%u",
                 (unsigned long long)st.ingested_bytes, (unsigned long long)st.dropped_bytes,
                 (unsigned long)st.pages_written, st.degraded, st.nor_failed, s_seq);
    }
}
