/* MTP spike (MTP-P): MTP + CDC composite on the ESP32-S3 with esp_tinyusb 3.0.0 (forked MTP layer, read-only, fake VFS).
 * Boot: GPIO 2 (MODE2, internal pull-up). High = normal USB Serial/JTAG (serial mode). Low = files mode.
 * Build flag -DSPIKE_FORCE_FILES=1: ignore the pin. Files mode always starts after a grace time (SPIKE_GRACE_MS),
 * in which the USB Serial/JTAG is alive, so that the board can be flashed.
 * CDC commands in files mode: DL (ROM download mode), REBOOT, INFO, LOG, any other line is echoed.
 * No NAND pins (GPIO 10 to 13) are touched. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h"
#include "hal/usb_serial_jtag_ll.h"
#include "tinyusb_default_config.h"
#include "tinyusb.h"
#include "tusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_mtp.h"
#include "fake_vfs.h"

#define MODE2_PIN 2
#ifndef SPIKE_GRACE_MS
#define SPIKE_GRACE_MS 10000
#endif
static const char *TAG = "spike";

/* ---- log ring buffer: in files mode the console is invisible, so the CDC command LOG shows the log ---- */
#define RING_SZ 6144
static char s_ring[RING_SZ];
static size_t s_ring_n;           /* total bytes ever written */
static vprintf_like_t s_old_vprintf;
static int ring_vprintf(const char *fmt, va_list ap)
{
    char tmp[200];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap2);
    va_end(ap2);
    for (int i = 0; i < n && i < (int)sizeof tmp - 1; i++) { s_ring[s_ring_n++ % RING_SZ] = tmp[i]; }
    return s_old_vprintf ? s_old_vprintf(fmt, ap) : n;
}
static void cdc_puts(const char *s) { tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s, strlen(s)); tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(200)); }
static void ring_dump(void)
{
    size_t start = s_ring_n > RING_SZ ? s_ring_n - RING_SZ : 0;
    char line[64]; size_t k = 0;
    for (size_t i = start; i < s_ring_n; i++) {
        char c = s_ring[i % RING_SZ];
        if (c == '\033') { c = '~'; }
        line[k++] = c;
        if (k == sizeof line - 1 || c == '\n') { line[k] = 0; cdc_puts(line); k = 0; }
    }
    if (k) { line[k] = 0; cdc_puts(line); }
    cdc_puts("END\r\n");
}

/* Give the USB pads back to USB Serial/JTAG (the mux lives in the RTC domain, a reset may not undo it). */
static void mux_to_jtag(void) { usb_serial_jtag_ll_phy_enable_external(false); }

static volatile int s_action;     /* 1 = REBOOT, 2 = DL */
static char s_line[64]; static int s_ll;
static void on_rx(int itf, cdcacm_event_t *ev)
{
    (void)ev;
    uint8_t buf[64]; size_t n = 0;
    while (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof buf, &n) == ESP_OK && n > 0) {
        for (size_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (s_ll == 0) { continue; }
                s_line[s_ll] = 0; s_ll = 0;
                if (!strcmp(s_line, "DL")) { cdc_puts("DL: rebooting to ROM download mode\r\n"); s_action = 2; }
                else if (!strcmp(s_line, "REBOOT")) { cdc_puts("REBOOT: restarting\r\n"); s_action = 1; }
                else if (!strcmp(s_line, "INFO")) {
                    char o[96]; snprintf(o, sizeof o, "INFO: spike files mode, up %llu s, heap %u\r\n",
                                         (unsigned long long)(esp_timer_get_time() / 1000000), (unsigned)esp_get_free_heap_size());
                    cdc_puts(o);
                }
                else if (!strcmp(s_line, "LOG")) { ring_dump(); }
                else { cdc_puts("ECHO: "); cdc_puts(s_line); cdc_puts("\r\n"); }
            } else if (s_ll < (int)sizeof s_line - 1) { s_line[s_ll++] = c; }
        }
    }
}

static void files_mode(void)
{
    ESP_ERROR_CHECK(fake_vfs_init());
    const tinyusb_mtp_driver_config_t dc = { .manufacturer = "IREC SRAD", .model = "DAQ MTP spike", .version = "0.1", .serial = "SPIKE0001",
                                             .friendly_name = "IREC DAQ" };
    ESP_ERROR_CHECK(tinyusb_mtp_install_driver(&dc));
    const tinyusb_mtp_storage_config_t sc = { .base_path = FAKE_BASE, .display_name = "DAQ", .removable = false,
                                              .total_bytes = FAKE_NAND_SIZE, .free_bytes = 0 };
    ESP_ERROR_CHECK(tinyusb_mtp_register_storage(&sc, NULL));
    const tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tc));
    const tinyusb_config_cdcacm_t cc = { .cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = on_rx };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cc));
    ESP_LOGI(TAG, "files mode running (MTP + CDC)");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_action) {
            vTaskDelay(pdMS_TO_TICKS(300));       /* let the CDC reply go out */
            /* MTP-P-8: drop D+ so the host sees a disconnect. Without it the pad pull-up stays on across the reset and
             * the host never re-enumerates (stale device 4041, board invisible). */
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            mux_to_jtag();
            if (s_action == 2) { REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT); }
            esp_restart();
        }
    }
}

void app_main(void)
{
    mux_to_jtag();                                   /* always start on the USB Serial/JTAG */
    s_old_vprintf = esp_log_set_vprintf(ring_vprintf);
    const gpio_config_t gc = { .pin_bit_mask = 1ULL << MODE2_PIN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&gc);
    vTaskDelay(pdMS_TO_TICKS(50));
    int lvl = gpio_get_level(MODE2_PIN);
#ifdef SPIKE_FORCE_FILES
    bool files = true;
#else
    bool files = (lvl == 0);
#endif
    ESP_LOGI(TAG, "boot: gpio2=%d files=%d force=%d", lvl,
#ifdef SPIKE_FORCE_FILES
             files, 1
#else
             files, 0
#endif
    );
    if (files) {
        for (int s = SPIKE_GRACE_MS / 1000; s > 0; s--) {   /* grace: USB Serial/JTAG alive, flash now */
            printf("SPIKE: files mode in %d s (gpio2=%d). Flash now if needed.\n", s, lvl);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        files_mode();
    }
    for (;;) {
        printf("SPIKE: serial mode, gpio2=%d (high = serial, low = files)\n", gpio_get_level(MODE2_PIN));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
