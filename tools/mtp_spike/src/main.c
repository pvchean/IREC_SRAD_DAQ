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
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#include "esp_rom_sys.h"
#include "soc/system_reg.h"
#include "soc/usb_serial_jtag_struct.h"
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

/* ---- USB Serial/JTAG pad safety (MTP-P-11). Read Docs/MTP/Spike_Report.md MTP-P-11 before you change anything here. ----
 * RULE 1: this file NEVER leaves USB_SERIAL_JTAG.conf0 with a non-default pull value. The only conf0 writes are
 *         phy_sel = 0 (mux_to_jtag), the pull fields (usj_pads_restore, only when they differ from the reset default) and
 *         usb_pad_enable = 1 (the reset default). The v1 build wrote dp_pullup=0 / dp_pulldown=1 to conf0 at every boot: the pad died.
 * RULE 2: no pad or PHY register is written at a normal boot (power-on, esp_restart, DL, REBOOT).
 * RULE 3: crash-reset disconnect (SPIKE_P3) is OFF by default. It never writes conf0. */
static void mux_to_jtag(void) { usb_serial_jtag_ll_phy_enable_external(false); }     /* RTC mux + USJ.phy_sel = USJ owns the PHY */

static bool usj_clock_on(void)   /* same test as usb_serial_jtag_ll_module_is_enabled(), without the RCC atomic macro */
{
    return (REG_READ(SYSTEM_PERIP_CLK_EN1_REG) & SYSTEM_USB_DEVICE_CLK_EN) && !(REG_READ(SYSTEM_PERIP_RST_EN1_REG) & SYSTEM_USB_DEVICE_RST);
}
static bool usj_pads_clean(void)  /* true = conf0 pull fields and pad enable are at the TRM reset default */
{
    return !USB_SERIAL_JTAG.conf0.pad_pull_override && USB_SERIAL_JTAG.conf0.dp_pullup && !USB_SERIAL_JTAG.conf0.dp_pulldown &&
           !USB_SERIAL_JTAG.conf0.dm_pullup && !USB_SERIAL_JTAG.conf0.dm_pulldown && USB_SERIAL_JTAG.conf0.usb_pad_enable;
}
/* Idempotent. Clean state or clock off: it writes NOTHING. Dirty state (a stale value from an older firmware): it writes the reset
 * default (dp_pu=1, others 0, override off, pad on). Returns true if it had to repair something. */
static bool usj_pads_restore(void)
{
    if (!usj_clock_on() || usj_pads_clean()) { return false; }
    const usb_serial_jtag_pull_override_vals_t n = { .dp_pu = true, .dm_pu = false, .dp_pd = false, .dm_pd = false };
    usb_serial_jtag_ll_phy_enable_pull_override(&n);       /* sane register values first, override on */
    usb_serial_jtag_ll_phy_disable_pull_override();        /* then give the pad back to the controller */
    usb_serial_jtag_ll_phy_enable_pad(true);
    return true;
}

#ifndef SPIKE_P3
#define SPIKE_P3 0     /* 1 = after a crash reset, make the host see a disconnect (see crash_disconnect). Default OFF. UNVERIFIED on hardware. */
#endif
#if SPIKE_P3
/* Replica of dcd_disconnect() of the fork (the same writes that the REBOOT/DL path already runs, proven in MTP-P-9.1): USB_WRAP pull override
 * (D+ pull-down) and DWC2 DCTL.SDIS. It touches ONLY when the USB OTG clock is on AND the RTC mux still maps the PHY to the OTG (stale state of
 * the crashed session). It never writes USB_SERIAL_JTAG.conf0. The USB_WRAP override stays on afterwards, as after tud_disconnect() in the
 * proven path; the PHY goes to the USJ next, and tinyusb resets USB_WRAP before it uses it again. */
static void crash_disconnect(void)
{
    const bool otg_clk = (REG_READ(SYSTEM_PERIP_CLK_EN0_REG) & SYSTEM_USB_CLK_EN) && !(REG_READ(SYSTEM_PERIP_RST_EN0_REG) & SYSTEM_USB_RST);
    const bool mux_otg = RTCCNTL.usb_conf.sw_hw_usb_phy_sel && RTCCNTL.usb_conf.sw_usb_phy_sel;
    if (!otg_clk || !mux_otg) { return; }
    usb_wrap_otg_conf_reg_t c = USB_WRAP.otg_conf;
    c.pad_pull_override = 1; c.dp_pullup = 0; c.dp_pulldown = 1; c.dm_pullup = 0; c.dm_pulldown = 1;
    USB_WRAP.otg_conf = c;
    REG_SET_BIT(0x60080000UL + 0x804, BIT(1));     /* DWC2 DCTL.SDIS (soft disconnect) */
    vTaskDelay(pdMS_TO_TICKS(500));                        /* as long as the proven REBOOT path */
}
#endif

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
                else if (!strcmp(s_line, "HANG")) { cdc_puts("HANG: blocking the USB task now, the watchdog must reset the chip\r\n"); for (;;) { } }
                else if (!strcmp(s_line, "ABORT")) { cdc_puts("ABORT: abort() now\r\n"); vTaskDelay(pdMS_TO_TICKS(200)); abort(); }
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
    tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    tc.task.size = 6144;                              /* MTP-P-10 P4 */
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
            usj_pads_restore();
            if (s_action == 2) { REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT); }
            esp_restart();
        }
    }
}

void app_main(void)
{
    const bool pads_were_dirty = usj_pads_restore();    /* FIRST, always: cleans a stale pad state of an older firmware (no write if clean) */
    const esp_reset_reason_t rr = esp_reset_reason();
#if SPIKE_P3
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) { crash_disconnect(); }
#endif
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
    ESP_LOGI(TAG, "reset reason %d, USJ pads were %s, P3 %d", (int)rr, pads_were_dirty ? "DIRTY (repaired)" : "clean", SPIKE_P3);
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
