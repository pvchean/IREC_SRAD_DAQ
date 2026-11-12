/* Transport of the text commands (cmd.c) and of the live stream (stream.c). One place chooses the port.
   Serial mode (MODE2 high, or a build with no DAQ_MTP): USB Serial/JTAG, exactly as before. cio_printf() is vprintf(), so
   the output goes through the same stdout (line end CR LF) as the old printf() calls.
   Files mode (DAQ_MTP build and MODE2 low): the USB Serial/JTAG driver is NOT installed (the PHY belongs to TinyUSB). The
   backend is a stub (writes are dropped, reads give no data) until the USB code calls cio_set_backend() with its CDC port.
   See Docs/MTP/Boot_Integration.md MTP-B-3. */
#ifndef CONSOLE_IO_H
#define CONSOLE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A pluggable backend. Both functions may be called from any task, but only one task writes and one task reads.
   write: give the bytes to the host; return the bytes accepted (0 up to len), or < 0 on an error. It waits at most timeout_ms.
   read:  return the bytes read (0 up to len); 0 if no data came in timeout_ms; < 0 on an error. It must wait (not spin). */
typedef struct {
    int (*write)(const void *buf, size_t len, uint32_t timeout_ms);
    int (*read)(void *buf, size_t len, uint32_t timeout_ms);
} cio_backend_t;

/* Choose the backend. Serial mode: install the USB Serial/JTAG driver (4096-byte TX ring, as cmd_start() did) and connect stdout
   to it. Files mode: install nothing; use the stub. Call once, before the first read. cmd_start() calls it. */
void cio_init(void);

/* Files mode only: replace the stub with the backend of the USB code (CDC). NULL = back to the stub. Ignored in serial mode. */
void cio_set_backend(const cio_backend_t *be);

bool cio_is_serial(void);

/* Same meaning as usb_serial_jtag_write_bytes() and usb_serial_jtag_read_bytes(), with the time in ms. */
int cio_write(const void *buf, size_t len, uint32_t timeout_ms);
int cio_read(void *buf, size_t len, uint32_t timeout_ms);

/* printf() and fflush(stdout) of the console. In files mode the text goes to cio_write() with CR LF line ends. */
int cio_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cio_flush(void);

#endif
