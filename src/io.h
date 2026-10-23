/* Mode pins, address pins, and the record button. */
#ifndef IO_H
#define IO_H

#include <stdbool.h>
#include <stdint.h>

void io_init(void);              /* pull-up, wait, read, then the button interrupt */
bool io_auto_mode(void);         /* MODE1 was high at power-up */
bool io_usb_files_mode(void);    /* MODE2 was low (not used yet) */
uint8_t io_addr(void);           /* ADDR2..ADDR0 */

/* Recording: auto mode starts on; the button flips it. Manual mode starts off; the REC command sets it. */
bool rec_get(void);
void rec_set(bool on);

#endif
