/* USB code of the files mode: strong files_mode_usb_start(). Merged from tools/mtp_spike/src/main.c (MTP-P-5, P-8, P-10, P-11).
   Rules (MTP-P-11, AGENT_RULES board-safety):
     RULE 1: this file NEVER leaves USB_SERIAL_JTAG.conf0 with a non-default pull value. The only conf0 writes are phy_sel = 0 (mux_to_jtag),
             the pull fields in usj_pads_restore() (only when they differ from the reset default) and usb_pad_enable = 1 (the reset default).
     RULE 2: no pad or PHY register is written at a normal boot (power-on, esp_restart, DL, REBOOT, LEAVE) except mux_to_jtag().
     RULE 3: no crash-reset disconnect code (the spike flag SPIKE_P3 is not merged: it was off by default and unverified).
   IN endpoints: EP0 + CDC (notification + data) + MTP (interrupt + data) = 5 of 5 TX FIFOs (dwc2_esp32.h ep_in_count = 5 counts EP0).
   REVIEW (MTP-V-7): no IN endpoint is free. NEVER add another USB class or endpoint. */
#ifdef DAQ_MTP_USB

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/system_reg.h"
#include "soc/usb_serial_jtag_struct.h"

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tinyusb_mtp.h"
#include "tusb.h"

#include "console_io.h"
#include "files_mode.h"
#include "mtp_store.h"
#include "mtp_vfs.h"
#include "usb_files.h"

#ifndef USB_GRACE_MS
#define USB_GRACE_MS 10000          /* USB Serial/JTAG stays alive this long before TinyUSB takes the PHY: the board can be flashed */
#endif
#define RING_SZ 6144
#define RX_SB_SZ 512

static const char *TAG = "usb_files";

static bool s_pads_were_dirty;
static esp_reset_reason_t s_reset_reason;
static volatile bool s_usb_up;

/* ---- USB Serial/JTAG pad safety (copy of the spike code, MTP-P-11) ---- */
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
/* Idempotent. Clean state or clock off: it writes NOTHING. Dirty state: it writes the reset default (dp_pu=1, others 0, override off, pad on).
   Returns true if it had to repair something. */
static bool usj_pads_restore(void)
{
    if (!usj_clock_on() || usj_pads_clean()) { return false; }
    const usb_serial_jtag_pull_override_vals_t n = { .dp_pu = true, .dm_pu = false, .dp_pd = false, .dm_pd = false };
    usb_serial_jtag_ll_phy_enable_pull_override(&n);       /* sane register values first, override on */
    usb_serial_jtag_ll_phy_disable_pull_override();        /* then give the pad back to the controller */
    usb_serial_jtag_ll_phy_enable_pad(true);
    return true;
}

void usb_files_early_init(void)
{
    s_pads_were_dirty = usj_pads_restore();    /* FIRST, always */
    s_reset_reason = esp_reset_reason();
    mux_to_jtag();                             /* always start on the USB Serial/JTAG */
}

void usb_files_prepare_restart(void)
{
    /* MTP-P-9.2: without tud_disconnect() the D+ pull-up stays on across the reset and the host keeps a stale device. */
    if (s_usb_up) {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    mux_to_jtag();
    usj_pads_restore();                        /* no-op when clean */
}

/* ---- log ring: in files mode the console is invisible, so the CDC command LOG shows the log ---- */
static char s_ring[RING_SZ];
static size_t s_ring_n;                        /* total bytes ever written */
static vprintf_like_t s_old_vprintf;
static int ring_vprintf(const char *fmt, va_list ap)
{
    char tmp[200];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap2);
    va_end(ap2);
    for (int i = 0; i < n && i < (int)sizeof tmp - 1; i++) { s_ring[s_ring_n++ % RING_SZ] = tmp[i]; }
    return s_old_vprintf ? s_old_vprintf(fmt, ap) : n;
}

void usb_files_log_start(void)
{
    s_old_vprintf = esp_log_set_vprintf(ring_vprintf);
    ESP_LOGI(TAG, "reset reason %d, USJ pads were %s", (int)s_reset_reason, s_pads_were_dirty ? "DIRTY (repaired)" : "clean");
}

/* ---- CDC as the console backend (cio_set_backend) ---- */
static StreamBufferHandle_t s_rx_sb;
static SemaphoreHandle_t s_tx_lock;

static int cdc_write(const void *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_usb_up || !tud_cdc_n_connected(0)) { return (int)len; }     /* nobody has the port open: drop, never block the command task */
    const uint8_t *p = buf;
    size_t left = len;
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    while (left) {
        size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, p, left);
        p += n;
        left -= n;
        int64_t rest = end - esp_timer_get_time();
        if (rest < 0) { rest = 0; }
        if (tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS((uint32_t)(rest / 1000) + 1U)) != ESP_OK && left) { break; }
        if (n == 0 && esp_timer_get_time() >= end) { break; }
    }
    xSemaphoreGive(s_tx_lock);
    return (int)(len - left);
}

static int cdc_read(void *buf, size_t len, uint32_t timeout_ms)
{
    return (int)xStreamBufferReceive(s_rx_sb, buf, len, pdMS_TO_TICKS(timeout_ms));
}

static const cio_backend_t s_cdc_be = { cdc_write, cdc_read };

/* ---- CDC receive: a line parser that acts on the way-back commands by itself (they must work when the cmd task is busy or dead),
   and gives every other line to the cmd task through the stream buffer ---- */
enum { A_NONE, A_REBOOT, A_DL, A_LEAVE, A_LOG };
static volatile int s_action;
static char s_line[24];
static size_t s_ll;
static bool s_pass;                            /* a long line: bytes go straight to the cmd task */

static void rx_forward(const void *p, size_t n) { (void)xStreamBufferSend(s_rx_sb, p, n, 0); }

static void on_rx(int itf, cdcacm_event_t *ev)
{
    (void)ev;
    uint8_t buf[64];
    size_t n = 0;
    while (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof buf, &n) == ESP_OK && n > 0) {
        for (size_t i = 0; i < n; i++) {
            const char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (s_pass) { rx_forward("\n", 1); s_pass = false; s_ll = 0; continue; }
                if (s_ll == 0) { continue; }
                s_line[s_ll] = 0;
                if (!strcmp(s_line, "DL")) { s_action = A_DL; }
                else if (!strcmp(s_line, "REBOOT")) { s_action = A_REBOOT; }
                else if (!strcmp(s_line, "LEAVE")) { s_action = A_LEAVE; }
                else if (!strcmp(s_line, "LOG")) { s_action = A_LOG; }
#ifdef USB_FILES_TEST_CMDS                                      /* POLISH (MTP-R): only in env daq_mtp_force (MTP-V-12) */
                else if (!strcmp(s_line, "HANG")) { for (;;) { } }     /* TEST ONLY (MTP-P-10): block the USB task, the 8 s task watchdog must reset the chip */
#endif
                else { s_line[s_ll++] = '\n'; rx_forward(s_line, s_ll); }
                s_ll = 0;
            } else if (s_pass) {
                rx_forward(&c, 1);
            } else if (s_ll < sizeof s_line - 2U) {
                s_line[s_ll++] = c;
            } else {                           /* too long for a way-back command: it is a normal command */
                rx_forward(s_line, s_ll);
                rx_forward(&c, 1);
                s_pass = true;
                s_ll = 0;
            }
        }
    }
}

static void cdc_puts(const char *s) { (void)cdc_write(s, strlen(s), 200); }

static void ring_dump(void)
{
    size_t start = s_ring_n > RING_SZ ? s_ring_n - RING_SZ : 0;
    char line[64];
    size_t k = 0;
    for (size_t i = start; i < s_ring_n; i++) {
        char c = s_ring[i % RING_SZ];
        if (c == '\033') { c = '~'; }
        if (c == '\n') { line[k++] = '\r'; }
        line[k++] = c;
        if (k >= sizeof line - 2U || c == '\n') { line[k] = 0; cdc_puts(line); k = 0; }
    }
    if (k) { line[k] = 0; cdc_puts(line); }
    /* REVIEW (MTP-V-9): the stack and heap margins on real hardware (the host test cannot show them). Reads only. */
    static const char *const names[] = { "TinyUSB", "mtp_vfs", "cmd", "main", "led" };
    static char d[192];                        /* static: ring_dump() runs only in the main task (3584-byte stack) */
    int n = snprintf(d, sizeof d, "DIAG heap_free=%u heap_min=%u stack_free_bytes:", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    for (size_t i = 0; i < sizeof names / sizeof names[0] && n > 0 && n < (int)sizeof d; i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h) { n += snprintf(d + n, sizeof d - (size_t)n, " %s=%u", names[i], (unsigned)uxTaskGetStackHighWaterMark(h)); }
    }
    cdc_puts(d);
    cdc_puts("\r\nEND\r\n");
}

/* The OpenSession hook of the fork: make the file lists again. Runs in the USB task. */
static void on_session_open(void)
{
    ESP_LOGI(TAG, "OpenSession: refresh the file lists");
    mtp_vfs_refresh();
}

static void usb_stack_start(void)
{
    s_rx_sb = xStreamBufferCreate(RX_SB_SZ, 1);
    s_tx_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(mtp_vfs_init());
    tinyusb_mtp_set_session_open_callback(on_session_open);
    const tinyusb_mtp_driver_config_t dc = { .manufacturer = "IREC SRAD", .model = "DAQ", .version = "1.0", .serial = NULL,
                                             .friendly_name = "IREC DAQ" };
    ESP_ERROR_CHECK(tinyusb_mtp_install_driver(&dc));
    const tinyusb_mtp_storage_config_t sc = { .base_path = MTP_VFS_BASE, .display_name = "DAQ", .removable = false,
                                              .total_bytes = mtp_vfs_capacity(), .free_bytes = 0 };
    ESP_ERROR_CHECK(tinyusb_mtp_register_storage(&sc, NULL));
    /* POLISH (MTP-R): no INFO trace per MTP transaction (it fills the LOG ring). Errors and warnings stay. */
    static const char *const quiet[] = { "tinyusb_mtp_transport", "tinyusb_mtp_ops", "tinyusb_mtp_read", "tinyusb_mtp_xfer",
                                         "tinyusb_mtp_write", "tinyusb_mtp_store", "tinyusb_mtp_prop", "tinyusb_mtp_codec" };
    for (unsigned i = 0; i < sizeof quiet / sizeof quiet[0]; i++) { esp_log_level_set(quiet[i], ESP_LOG_WARN); }
    tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    tc.task.size = 8192;            /* REVIEW (MTP-V-4): was 6144 (MTP-P-10 P4). The MTP read runs fread -> VFS -> PFS read -> RS decode, plus a log line, in this task */
    ESP_ERROR_CHECK(tinyusb_driver_install(&tc));
    s_usb_up = true;
    const tinyusb_config_cdcacm_t cc = { .cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = on_rx };
    if (tinyusb_cdcacm_init(&cc) != ESP_OK) {             /* REVIEW (MTP-V-10): D+ is up now. abort() would leave a stale device; drop D+ first */
        ESP_LOGE(TAG, "CDC init failed: restart");
        usb_files_prepare_restart();
        esp_restart();
    }
    cio_set_backend(&s_cdc_be);                           /* the cmd task now reads and writes the CDC port */
    ESP_LOGI(TAG, "files mode running (MTP + CDC)");
}

/* Strong definition: it replaces the weak default of files_mode.c. It does not return. */
void files_mode_usb_start(void)
{
    /* Grace window: the USB Serial/JTAG is alive (303a:1001) and the board can be flashed with a plain upload. */
    for (int s = USB_GRACE_MS / 1000; s > 0; s--) {
        ESP_LOGW(TAG, "files mode: USB MTP starts in %d s. Flash now if needed.", s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    usb_stack_start();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const int a = s_action;
        if (!a) { continue; }
        s_action = A_NONE;
        switch (a) {
        case A_LOG:    ring_dump(); break;
        case A_REBOOT: cdc_puts("REBOOT:OK\r\n"); files_mode_restart(FM_REBOOT);
        case A_DL:     cdc_puts("DL:OK rebooting to ROM download mode\r\n"); files_mode_restart(FM_DOWNLOAD);
        case A_LEAVE:  cdc_puts("LEAVE:OK\r\n"); files_mode_restart(FM_LEAVE);
        default: break;
        }
    }
}

#else
typedef int usb_files_not_built;   /* the file has no code when DAQ_MTP_USB is not set */
#endif
