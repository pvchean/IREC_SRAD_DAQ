#ifdef DAQ_PROFILE
#include "profile.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads1115.h"
#include "board.h"
#include "nau7802.h"
#include "sd_log.h"

#define MAGIC 0xB007F00DU

/* Kept over a software reset. After a power-on or a USB reset the magic is not valid. */
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint64_t s_restart_rtc_us;
static RTC_NOINIT_ATTR uint32_t s_want_wrap;

typedef struct {
    uint32_t xfers, delays, os_delays, reads;
    uint64_t xfer_bytes, xfer_us, delay_req_us, delay_us, os_delay_us, read_us;
} stats_t;

static bool s_inited, s_wrap;
static pfs_ctx_t *s_pfs;
static int64_t s_t[P_COUNT];
static int64_t s_pre_app_us = -1;
static stats_t s_st, s_at_mount;

static const pfs_os_hal_t *s_os_real;
static pfs_os_hal_t s_os_w;
static const spi_bus_hal_t *s_bus_real;
static spi_bus_hal_t s_bus_w;
static const nand_hal_t *s_nand_real;
static nand_hal_t s_nand_w;

static void init_once(void)
{
    if (s_inited) { return; }
    s_inited = true;
    for (int i = 0; i < P_COUNT; i++) { s_t[i] = -1; }
    if (s_magic == MAGIC && esp_reset_reason() == ESP_RST_SW) {
        s_pre_app_us = (int64_t)(esp_clk_rtc_time() - s_restart_rtc_us) - esp_timer_get_time();
        s_wrap = s_want_wrap != 0U;
    }
    s_magic = 0;
}

void prof_mark(int id)
{
    init_once();
    if (s_t[id] < 0) { s_t[id] = esp_timer_get_time(); }
    if (id == P_MOUNT1) { s_at_mount = s_st; }
}

static void w_os_delay_ms(uint32_t ms)
{
    int64_t t0 = esp_timer_get_time();
    s_os_real->delay_ms(ms);
    s_st.os_delays++;
    s_st.os_delay_us += (uint64_t)(esp_timer_get_time() - t0);
}

static pfs_hal_status_t w_transfer(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    int64_t t0 = esp_timer_get_time();
    pfs_hal_status_t r = s_bus_real->transfer(tx, tx_len, rx, rx_len);
    s_st.xfers++;
    s_st.xfer_bytes += tx_len + rx_len;
    s_st.xfer_us += (uint64_t)(esp_timer_get_time() - t0);
    return r;
}

static void w_delay_us(uint32_t us)
{
    int64_t t0 = esp_timer_get_time();
    s_bus_real->delay_us(us);
    s_st.delays++;
    s_st.delay_req_us += us;
    s_st.delay_us += (uint64_t)(esp_timer_get_time() - t0);
}

static nand_status_t w_read_page(uint16_t pba, uint16_t page, uint8_t *main_buf, uint8_t *user_oob, nand_ecc_t *ecc)
{
    int64_t t0 = esp_timer_get_time();
    nand_status_t r = s_nand_real->read_page(pba, page, main_buf, user_oob, ecc);
    s_st.reads++;
    s_st.read_us += (uint64_t)(esp_timer_get_time() - t0);
    return r;
}

const pfs_os_hal_t *prof_wrap_os(const pfs_os_hal_t *os)
{
    init_once();
    if (!s_wrap) { return os; }
    s_os_real = os;
    s_os_w = *os;
    s_os_w.delay_ms = w_os_delay_ms;
    return &s_os_w;
}

const spi_bus_hal_t *prof_wrap_bus(const spi_bus_hal_t *bus)
{
    init_once();
    if (!s_wrap) { return bus; }
    s_bus_real = bus;
    s_bus_w = *bus;
    s_bus_w.transfer = w_transfer;
    s_bus_w.delay_us = w_delay_us;
    return &s_bus_w;
}

const nand_hal_t *prof_wrap_nand(const nand_hal_t *nand)
{
    init_once();
    if (!s_wrap) { return nand; }
    s_nand_real = nand;
    s_nand_w = *nand;
    s_nand_w.read_page = w_read_page;
    return &s_nand_w;
}

static void watch_task(void *arg)
{
    pfs_ctx_t *pfs = (pfs_ctx_t *)arg;
    for (int n = 0; n < 8000; n++) {               /* 5 ms steps, at most 40 s */
        pfs_status_t st;
        if (s_t[P_ADS_RUN] < 0 && ads1115_running()) { prof_mark(P_ADS_RUN); }
        if (s_t[P_NAU_RUN] < 0 && nau7802_running()) { prof_mark(P_NAU_RUN); }
        if (s_t[P_SD_MOUNTED] < 0 && sd_log_mounted()) { prof_mark(P_SD_MOUNTED); }
        pfs_get_status(pfs, &st);
        if (s_t[P_INDEX_READY] < 0 && st.index_ready) { prof_mark(P_INDEX_READY); }
        if (s_t[P_ADS_RUN] >= 0 && s_t[P_NAU_RUN] >= 0 && s_t[P_SD_MOUNTED] >= 0 && s_t[P_INDEX_READY] >= 0) { break; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(NULL);
}

void prof_start(pfs_ctx_t *pfs)
{
    s_pfs = pfs;
    xTaskCreatePinnedToCore(watch_task, "prof", 3072, pfs, 2, NULL, 1);
}

static double ms(int id) { return s_t[id] < 0 ? -1.0 : (double)s_t[id] / 1000.0; }

static void print_stats(const char *what, const stats_t *s)
{
    printf("BUSY: %s spi_xfers=%" PRIu32 " spi_bytes=%" PRIu64 " spi_ms=%.1f busy_delay_calls=%" PRIu32
           " busy_delay_requested_ms=%.1f busy_delay_real_ms=%.1f nand_reads=%" PRIu32 " nand_read_ms=%.1f"
           " os_sleeps=%" PRIu32 " os_sleep_ms=%.1f\n",
           what, s->xfers, s->xfer_bytes, (double)s->xfer_us / 1000.0, s->delays, (double)s->delay_req_us / 1000.0,
           (double)s->delay_us / 1000.0, s->reads, (double)s->read_us / 1000.0, s->os_delays,
           (double)s->os_delay_us / 1000.0);
}

void prof_print(void)
{
    init_once();
    printf("BOOT: nand_spi_hz=%u wrap=%d pre_app_ms=%.1f | ms since the start of esp_timer: app_main=%.1f io=%.1f chain=%.1f"
           " early_init=%.1f sensors_started=%.1f mount_start=%.1f mount_end=%.1f writer=%.1f stream=%.1f sd_start=%.1f"
           " cmd=%.1f | ready: ads=%.1f load_cell=%.1f sd_mounted=%.1f file_index=%.1f | mount_ms=%.1f\n",
           (unsigned)NAND_SPI_HZ, s_wrap, s_pre_app_us < 0 ? -1.0 : (double)s_pre_app_us / 1000.0, ms(P_APP_MAIN), ms(P_IO), ms(P_CHAIN), ms(P_EARLY),
           ms(P_SENSORS), ms(P_MOUNT0), ms(P_MOUNT1), ms(P_WRITER), ms(P_STREAM), ms(P_SD), ms(P_CMD), ms(P_ADS_RUN),
           ms(P_NAU_RUN), ms(P_SD_MOUNTED), ms(P_INDEX_READY), ms(P_MOUNT1) - ms(P_MOUNT0));
    if (s_wrap) {
        print_stats("until_mount_end", &s_at_mount);
        print_stats("until_now", &s_st);
    }
    if (s_pfs) {                                   /* nothing here may be above 0 after a read-only test: no write, no erase */
        pfs_status_t st;
        pfs_get_status(s_pfs, &st);
        printf("STATUS2: mounted=%d path=%u index_ready=%d degraded=%d no_storage=%d nor_failed=%d map_mismatch=%d"
               " pages_written=%" PRIu32 " ingested=%" PRIu64 " nor_commits=%" PRIu32 " nor_checkpoints=%" PRIu32
               " nor_failures=%" PRIu32 " stale_blocks=%" PRIu32 " recycled=%" PRIu32 " migrations=%" PRIu32
               " abandoned=%" PRIu32 " verify_failures=%" PRIu32 " rs_repairs=%" PRIu32 " read_failures=%" PRIu32
               " nand_ecc_warnings=%" PRIu32 " mount_skipped_pages=%" PRIu32 " mount_bad_pages=%" PRIu32
               " unidentified=%" PRIu32 " usable=%u retired=%u quarantined=%u active_file=%u\n",
               st.mounted, st.mount_path, st.index_ready, st.degraded, st.no_storage, st.nor_failed, st.map_mismatch,
               st.pages_written, st.ingested_bytes, st.nor_commits, st.nor_checkpoints, st.nor_failures, st.stale_blocks,
               st.recycled_blocks, st.migrations, st.abandoned_blocks, st.verify_failures, st.rs_repairs, st.read_failures,
               st.nand_ecc_warnings, st.mount_skipped_pages, st.mount_bad_pages, st.unidentified_blocks, st.usable_blocks,
               st.retired_blocks, st.quarantined_blocks, st.active_file_id);
    }
}

void prof_reboot(int wrap)
{
    s_magic = MAGIC;
    s_want_wrap = wrap ? 1U : 0U;
    s_restart_rtc_us = esp_clk_rtc_time();
    esp_restart();
}
#endif
