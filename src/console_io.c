#include "console_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io.h"

#define LINE_TX_MS 50U                    /* files mode: time that cio_printf() waits for one piece */

/* Files mode, before the USB code has a port: drop writes, and give no data after the wait (so the cmd task does not spin). */
static int stub_write(const void *buf, size_t len, uint32_t timeout_ms) { (void)buf; (void)timeout_ms; return (int)len; }
static int stub_read(void *buf, size_t len, uint32_t timeout_ms)
{
    (void)buf; (void)len;
    vTaskDelay(pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1U));
    return 0;
}
static const cio_backend_t s_stub __attribute__((unused)) = { stub_write, stub_read };

static const cio_backend_t *volatile s_be;           /* NULL = USB Serial/JTAG (serial mode) */

void cio_init(void)
{
#ifdef DAQ_MTP
    if (io_usb_files_mode()) { s_be = &s_stub; return; }
#endif
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;                      /* CHUNK data (written in 1 KB pieces) */
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
}

void cio_set_backend(const cio_backend_t *be)
{
#ifdef DAQ_MTP
    if (io_usb_files_mode()) { s_be = be ? be : &s_stub; }
#else
    (void)be;
#endif
}

bool cio_is_serial(void) { return s_be == NULL; }

int cio_write(const void *buf, size_t len, uint32_t timeout_ms)
{
    const cio_backend_t *be = s_be;
    if (!be) { return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(timeout_ms)); }
    return be->write(buf, len, timeout_ms);
}

int cio_read(void *buf, size_t len, uint32_t timeout_ms)
{
    const cio_backend_t *be = s_be;
    if (!be) { return usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(timeout_ms)); }
    return be->read(buf, len, timeout_ms);
}

/* Files mode: text with LF line ends goes out with CR LF (the serial mode gets the same from the stdout of the C library). */
static void put_text(const cio_backend_t *be, const char *s, size_t n)
{
    char out[128];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') { out[k++] = '\r'; }
        out[k++] = s[i];
        if (k >= sizeof out - 1U || i + 1U == n) { (void)be->write(out, k, LINE_TX_MS); k = 0; }
    }
}

int cio_printf(const char *fmt, ...)
{
    const cio_backend_t *be = s_be;
    va_list ap;
    int n;
    va_start(ap, fmt);
    if (!be) {
        n = vprintf(fmt, ap);
    } else {
        char buf[256];
        va_list ap2;
        va_copy(ap2, ap);
        n = vsnprintf(buf, sizeof buf, fmt, ap);
        if (n >= (int)sizeof buf) {                 /* a long line (INFO, CHECK): format again in heap memory */
            char *big = malloc((size_t)n + 1U);
            if (big) { (void)vsnprintf(big, (size_t)n + 1U, fmt, ap2); put_text(be, big, (size_t)n); free(big); }
        } else if (n > 0) {
            put_text(be, buf, (size_t)n);
        }
        va_end(ap2);
    }
    va_end(ap);
    return n;
}

void cio_flush(void)
{
    if (!s_be) { fflush(stdout); }                   /* files mode: cio_printf() has no buffer */
}
