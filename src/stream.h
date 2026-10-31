/* Live readings on the USB serial port. PT: one voltage per line ("%.3f"), as the sketch printed them.
   Load cell: "L:<raw count>". The letter makes it a text line, so a reader that knows only the PT skips it.
   Off at power-up. The commands MATLAB PING and STREAM ON turn it on; STREAM OFF turns it off. */
#ifndef STREAM_H
#define STREAM_H

#include <stdbool.h>

#include "record.h"

void stream_start(void);
void stream_enable(bool on);        /* also mutes the ESP-IDF log while on; off returns after the last number line */
bool stream_enabled(void);
void stream_push(const record_t *r);   /* never waits; one writer at a time */

#endif
