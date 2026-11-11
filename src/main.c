/* IREC SRAD DAQ: ADS1115 (860 SPS) and NAU7802 (320 SPS) -> ISR -> one task each -> sink -> pfs_ingest() -> Paranoid FS
   on the ZD35Q1GC. The sink also feeds the SD card, the chain, and the live stream.
   The start-up follows firmware/main_esp32s3.c of Paranoid-FS (HAL-V3-7.6). */
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pfs.h"
#include "pfs_esp32s3.h"
#include "pfs_zd35q1gc.h"

#include "ads1115.h"
#include "board.h"
#include "cal.h"
#include "chain.h"
#include "cmd.h"
#include "files_mode.h"
#include "io.h"
#include "led_sr.h"
#include "mtp_store_pfs.h"
#include "nand_test.h"
#include "nor_ro.h"
#include "nau7802.h"
#include "profile.h"
#include "record.h"
#include "sd_log.h"
#include "stream.h"
#include "telem.h"

#define WRITER_STACK 8192U
#define SCAN_STACK   4096U

static const char *TAG = "daq";


static pfs_ctx_t   *s_pfs;
static StaticTask_t s_writer_tcb, s_scan_tcb;
static StackType_t  s_writer_stack[WRITER_STACK], s_scan_stack[SCAN_STACK];
static uint16_t     s_seq;                                     /* each sensor has its own seq (record.h) */
/* The sensor tasks run at the same time. pfs_ingest() has its own lock, but the stream buffers and the chain batches
   allow one writer. The lock is held for one record only, so a sensor task never waits for long. */
static SemaphoreHandle_t s_sink_lock;
static StaticSemaphore_t s_sink_lock_struct;

static bool s_was_rec;
static volatile bool s_fm_error;               /* files mode: the NAND did not mount */
static int64_t s_cal_last_us;

/* The calibration block (cal.h): 6 META records one after another, so the sink lock is held. It goes into the PFS file and the
   SD file only while recording, and to the chain always. It comes before the next sample at the start of a recording, after a
   CAL command or PFSFLUSH (cal_request), and every CAL_PERIOD_S seconds. */
static void emit_cal(bool rec)
{
    record_t b[CAL_BLOCK_RECS];
    cal_make_block(b);
    if (rec) {
        for (unsigned i = 0; i < CAL_BLOCK_RECS; i++) {
            (void)pfs_ingest(s_pfs, (const uint8_t *)&b[i], sizeof b[i]);
            sd_log_push(&b[i]);
        }
    }
    chain_push_cal(b, CAL_BLOCK_RECS);
    s_cal_last_us = esp_timer_get_time();
}

static void sink(const record_t *r)
{
    xSemaphoreTake(s_sink_lock, portMAX_DELAY);
    bool rec = rec_get();
    if (cal_take_request() || (rec && !s_was_rec) || esp_timer_get_time() - s_cal_last_us >= (int64_t)CAL_PERIOD_S * 1000000) {
        emit_cal(rec);
    }
    s_was_rec = rec;
    if (!telem_hide_local()) { stream_push(r); }               /* live readings, only after MATLAB PING; not on the DOWNLOAD DAQ */
    telem_push(r);                                             /* WiFi UPLOAD mode only: every Nth sample to the ring, never waits */
    if (rec_is_lc(r)) { chain_push_lc(r); }                    /* to the other boards, also when not recording */
    else { chain_push_ads(r); }
    if (rec) {                                                 /* recording = saving: PFS and SD only while on */
        (void)pfs_ingest(s_pfs, (const uint8_t *)r, sizeof *r);   /* never waits; the status counts drops */
        sd_log_push(r);                                        /* the backup copy */
    }
    xSemaphoreGive(s_sink_lock);
}

static void on_sample(const ads_sample_t *s)
{
    record_t r = { rec_ts(s->timestamp_us), s->reading, s_seq };
    s_seq = (uint16_t)((s_seq + 1U) & REC_PT_SEQ_MASK);
    sink(&r);
}

#if defined(DAQ_SIM) || !defined(DAQ_TOGGLE)      /* the toggle test does not start the load cell */
static uint8_t      s_lc_seq;
static void on_load_cell(const nau_sample_t *s)
{
    record_t r = rec_make_lc(rec_ts(s->timestamp_us), s->reading, s_lc_seq);
    s_lc_seq = (uint8_t)((s_lc_seq + 1U) & REC_LC_SEQ_MASK);
    sink(&r);
}
#endif

#ifdef DAQ_SIM
/* Test build: sawtooth signals in place of the sensors. PT 1 kHz, load cell 333 Hz, with counts of both signs and more
   than 16 bits. They use the same path (sink -> pfs_ingest). The low 7 bits of a load cell count are its seq. */
static void sim_task(void *a)
{
    (void)a;
    TickType_t t = xTaskGetTickCount();
    for (uint32_t i = 0, n = 0;; i++) {
        ads_sample_t s = { (uint32_t)esp_timer_get_time(), (int16_t)(i & 0x7FFF) };
        on_sample(&s);
        if (i % 3U == 0U) {
            nau_sample_t l = { (uint32_t)esp_timer_get_time(), (int32_t)(n++ & 0x3FFFFFU) - 0x200000 };
            on_load_cell(&l);
        }
        vTaskDelayUntil(&t, 1);
    }
}

/* Read the active file back and check that each sensor's seq counts up with no gap, and its reading fits its seq. */
static void readback_check(void)
{
    pfs_status_t st;
    pfs_file_info_t fi;
    uint8_t buf[512];
    size_t got = 0;
    uint16_t expect = 0;
    uint8_t lc_expect = 0;
    bool have = false, have_lc = false;
    uint32_t checked = 0, gaps = 0, lc_checked = 0, lc_gaps = 0, meta = 0, blocks = 0, bad_blocks = 0;
    cal_parser_t cp = { 0 };
    cal_t seen, now;

    pfs_get_status(s_pfs, &st);
    if (pfs_file_stat(s_pfs, st.active_file_id, &fi) != PFS_OK) { ESP_LOGE(TAG, "READBACK: no file"); return; }
    for (uint64_t off = 0; off + sizeof buf <= fi.size_bytes && checked < 2000; off += sizeof buf) {
        if (pfs_read(s_pfs, st.active_file_id, off, buf, sizeof buf, &got) != PFS_OK || got != sizeof buf) { break; }
        for (size_t i = 0; i + sizeof(record_t) <= got; i += sizeof(record_t)) {
            record_t r;
            memcpy(&r, buf + i, sizeof r);
            if (cal_parse(&cp, &r, &seen)) {                   /* a whole block: it must hold the values that the DAQ has */
                cal_get(&now);
                blocks++;
                if (memcmp(&seen.a, &now.a, sizeof now.a) || memcmp(&seen.b, &now.b, sizeof now.b) || seen.flags != now.flags) { bad_blocks++; }
            }
            if (rec_is_meta(&r)) { meta++; continue; }
            if (rec_is_lc(&r)) {
                uint8_t q = rec_lc_seq(&r);
                if (!have_lc) { lc_expect = q; have_lc = true; }
                if (q != lc_expect || (uint8_t)(rec_lc_count(&r) & REC_LC_SEQ_MASK) != q) { lc_gaps++; lc_expect = q; }
                lc_expect = (uint8_t)((lc_expect + 1U) & REC_LC_SEQ_MASK); lc_checked++;
                continue;
            }
            if (!have) { expect = r.seq; have = true; }
            if (r.seq != expect || (uint16_t)r.reading != r.seq) { gaps++; expect = r.seq; }
            expect = (uint16_t)((expect + 1U) & REC_PT_SEQ_MASK); checked++;
        }
        /* a 512-byte block holds 64 records: the record grid stays aligned */
    }
    ESP_LOGI(TAG, "READBACK: file %u size=%llu records_checked=%lu errors=%lu lc_checked=%lu lc_errors=%lu"
             " meta=%lu cal_blocks=%lu cal_block_errors=%lu",
             (unsigned)st.active_file_id, (unsigned long long)fi.size_bytes, (unsigned long)checked, (unsigned long)gaps,
             (unsigned long)lc_checked, (unsigned long)lc_gaps, (unsigned long)meta, (unsigned long)blocks,
             (unsigned long)bad_blocks);
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
        /* The load cell is optional: only a load cell that stopped after it ran is an error. */
        bool err = !s_pfs || st.degraded || st.no_storage || st.nor_failed || st.dropped_bytes > 0 ||
                   (!sensor && n > 60) || nau7802_fault();
        if (files_mode_active()) { err = s_fm_error; }          /* no sensors in files mode; the mount decides (MTP-B-2) */
        led_set(LED_WORKING, (n / 8) % 2 == 0);
        led_set(LED_RECORDING, rec_get());
        led_set(LED_AUTOREC, io_auto_mode());
        led_set(LED_WIFI, telem_link_up());
        led_set(LED_NAND, st.mounted && !st.degraded && !st.no_storage && st.verify_failures == 0);
        led_set(LED_SD, sd_log_mounted());
        led_set(LED_ERROR, err);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#ifdef DAQ_TOGGLE
/* Test signal for the chain test: GPIO 16 (I2C1 RDY) goes through the level shifter to the sensor line of both
   boards. Only the board with device ID DAQ_TOGGLE_ID drives it; the other board leaves the pin as an input.
   0 V / 3.3 V square wave, DAQ_TOGGLE_MS in each state. The readings step by about 17,600 counts. */
#ifndef DAQ_TOGGLE_ID
#define DAQ_TOGGLE_ID 3
#endif
#ifndef DAQ_TOGGLE_MS
#define DAQ_TOGGLE_MS 250
#endif
static void toggle_task(void *a)
{
    (void)a;
    gpio_reset_pin(I2C1_RDY_PIN);
    gpio_set_direction(I2C1_RDY_PIN, GPIO_MODE_INPUT_OUTPUT);     /* output, and the pad level can be read back */
    ESP_LOGI(TAG, "toggle: driving GPIO %d, %d ms per state", I2C1_RDY_PIN, DAQ_TOGGLE_MS);
    for (int level = 0, n = 0;; level ^= 1, n++) {
        gpio_set_level(I2C1_RDY_PIN, level);
        vTaskDelay(pdMS_TO_TICKS(DAQ_TOGGLE_MS));
        if (n < 8 || n % 20 == 0) { ESP_LOGI(TAG, "toggle: set=%d pad=%d", level, gpio_get_level(I2C1_RDY_PIN)); }
    }
}
#endif

static void writer_task(void *a) { pfs_writer_run((pfs_ctx_t *)a); }
static void scan_task(void *a)   { pfs_scan_run((pfs_ctx_t *)a); vTaskDelete(NULL); }

#ifdef DAQ_MTP
/* Files-mode boot (MTP-B-2): a read-only drive. No sensors, no chain, no stream, no SD logger, no WiFi, no pfs_writer, no
   recording. Nothing here calls pfs_writer_run(), pfs_ingest(), pfs_flush(), sd_log_start(), or stream_start(). */
#define FM_MOUNT_TRIES 10
static void files_mode_main(const pfs_config_t *cfg, const pfs_os_hal_t *os, const spi_bus_esp32s3_cfg_t *bus_cfg,
                            const pfs_nor_littlefs_cfg_t *nor_cfg)
{
    files_mode_set_active(true);
    io_rec_disable();                                           /* also done in io_init(); the second guard */
    ESP_LOGW(TAG, "FILES MODE (read only): no recording, no sensors, no WiFi");
    xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 1, NULL, 0);

    if (pfs_early_init(&s_pfs, os, cfg) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_early_init failed");
        s_pfs = NULL;                                           /* the LED task shows the error */
        s_fm_error = true;
    } else {
        const spi_bus_hal_t *bus  = spi_bus_esp32s3_get(bus_cfg);
        const nand_hal_t    *nand = nand_zd35q1gc_bind(bus);
        const pfs_nor_hal_t *nor  = nor_ro_littlefs_get(nor_cfg);        /* read only, never formats */
        pfs_err_t e = PFS_ERR_INVALID_ARG;
        for (int i = 0; i < FM_MOUNT_TRIES && e != PFS_OK; i++) {
            e = (nand && nor) ? mtp_pfs_mount_readonly(s_pfs, nand, nor) : PFS_ERR_INVALID_ARG;
            if (e != PFS_OK) { ESP_LOGE(TAG, "read-only mount failed (%d), try again in 1 s", (int)e); vTaskDelay(pdMS_TO_TICKS(1000)); }
        }
        if (e == PFS_OK) {
            (void)xTaskCreateStaticPinnedToCore(scan_task, "pfs_scan", SCAN_STACK, s_pfs, 2, s_scan_stack, &s_scan_tcb, 1);
            (void)mtp_store_pfs_init(s_pfs);
            ESP_LOGI(TAG, "NAND mounted read only: refused writes nand+nor=%lu, NOR refused=%lu",
                     (unsigned long)mtp_pfs_blocked_writes(), (unsigned long)mtp_pfs_blocked_nor_writes());
        } else {
            s_fm_error = true;                                  /* the SD folder can still work */
        }
    }
    (void)mtp_store_sd_init();
    if (s_pfs) { cmd_start(s_pfs); }                            /* the serial commands on the console backend (stub until USB) */
    files_mode_usb_start();                                     /* the USB code; the weak default idles */
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
#endif


void app_main(void)
{
    /* io_init() ran above: the ADDR2..ADDR0 pins (0..7) are the device ID in each PFS page header. */
    PROF(P_APP_MAIN);
#ifdef DAQ_NANDREAD
    nand_read_test();                          /* reads only, no mount */
#endif
    const pfs_config_t cfg = { .device_id = io_addr(), .writer_timeout_ms = 1000U, .max_ingest_packet = 1024U };
    const pfs_os_hal_t *os = PROF_OS(pfs_os_esp32s3_get());
    spi_bus_esp32s3_cfg_t bus_cfg = {
        .mosi = NAND_MOSI_PIN, .miso = NAND_MISO_PIN, .sclk = NAND_CLK_PIN, .cs = NAND_CS_PIN,
        .wp = NAND_WP_PIN, .hold = NAND_HOLD_PIN, .clock_hz = NAND_SPI_HZ, .log = os->log
    };
    static const pfs_nor_littlefs_cfg_t nor_cfg = { .partition_label = "pfs_nor", .base_path = "/pfs" };

    led_sr_init();
    io_init();                                 /* the level of the mode pins sets recording at power-up */
    PROF(P_IO);
#ifdef DAQ_MTP
    if (io_usb_files_mode()) { files_mode_main(&cfg, os, &bus_cfg, &nor_cfg); }   /* never returns; nothing below runs */
#endif
    cal_init();                                /* the calibration values come from the flash */
    telem_init();                              /* the WiFi mode from the flash (OFF = nothing else happens); no radio yet */
    xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 1, NULL, 0);
#ifdef DAQ_TOGGLE
    if (io_addr() == DAQ_TOGGLE_ID) { xTaskCreatePinnedToCore(toggle_task, "toggle", 2048, NULL, 1, NULL, 0); }
#endif
    chain_start();                             /* the packets of other boards pass through from the start */
    PROF(P_CHAIN);

    if (pfs_early_init(&s_pfs, os, &cfg) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_early_init failed");
        return;
    }
    PROF(P_EARLY);
#ifdef DAQ_PROFILE
    prof_start(s_pfs);                         /* before the mount: marks when the sensors give samples */
#endif
    const spi_bus_hal_t *bus  = PROF_BUS(spi_bus_esp32s3_get(&bus_cfg));
    const nand_hal_t    *nand = PROF_NAND(nand_zd35q1gc_bind(bus));
    const pfs_nor_hal_t *nor  = pfs_nor_littlefs_get(&nor_cfg);

    s_sink_lock = xSemaphoreCreateMutexStatic(&s_sink_lock_struct);

    /* The sensors start first: pfs_ingest() buffers the data during the mount (HL-V3-9.1). */
#ifdef DAQ_SIM
    xTaskCreatePinnedToCore((TaskFunction_t)sim_task, "sim", 4096, NULL, 6, NULL, 1);
#else
    if (ads1115_start(on_sample) != 0) { ESP_LOGE(TAG, "sensor not started"); }
#ifndef DAQ_TOGGLE                         /* the toggle test drives the DRDY pin */
    if (nau7802_start(on_load_cell) != 0) { ESP_LOGE(TAG, "load cell not started"); }
#endif
#endif

    PROF(P_SENSORS);
    PROF(P_MOUNT0);
    while (pfs_mount(s_pfs, nand, nor) != PFS_OK) {
        ESP_LOGE(TAG, "pfs_mount failed, try again in 1 s");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    PROF(P_MOUNT1);
    (void)xTaskCreateStaticPinnedToCore(writer_task, "pfs_writer", WRITER_STACK, s_pfs, 5,
                                        s_writer_stack, &s_writer_tcb, 1);
    (void)xTaskCreateStaticPinnedToCore(scan_task, "pfs_scan", SCAN_STACK, s_pfs, 2,
                                        s_scan_stack, &s_scan_tcb, 1);

    PROF(P_WRITER);
    stream_start();
    PROF(P_STREAM);
    sd_log_start(s_pfs);
    PROF(P_SD);
    cmd_start(s_pfs);
    PROF(P_CMD);
    telem_start();                             /* after the mount: WiFi (UPLOAD or DOWNLOAD mode only) on core 0 */

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
