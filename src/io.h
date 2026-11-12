/* Mode pins, address pins, and the record button. */
#ifndef IO_H
#define IO_H

#include <stdbool.h>
#include <stdint.h>

void io_init(void);              /* pull-up, wait, read, then the button interrupt */
bool io_auto_mode(void);         /* MODE1 was high at power-up */
bool io_usb_files_mode(void);    /* MODE2 was low, and no files_mode_leave() before this boot. Used by the daq_mtp build only (files_mode.h) */
void io_request_serial_once(void);   /* the next boot reads MODE2 as high, once (RTC no-init flag; files_mode_leave() calls it) */
uint8_t io_addr(void);           /* ADDR2..ADDR0 */

/* Recording: auto mode starts on; the button flips it. Manual mode starts off; the REC command sets it. */
bool rec_get(void);
void rec_set(bool on);           /* rec_set(true) does nothing when rec_locked() */

/* daq_mtp build (-DDAQ_MTP=1): recording is locked for the whole run. io_init() does it, so MODE1 high at boot or a button press
   cannot start it. io_rec_disable() does the same at run time (files mode calls it too). It removes the MODE1 interrupt. */
void io_rec_disable(void);
bool rec_locked(void);

#endif
