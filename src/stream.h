/* Live readings on the USB serial port, one voltage per line ("%.3f"), as the sketch printed them.
   Off at power-up. The command MATLAB PING turns it on; STREAM OFF turns it off. */
#ifndef STREAM_H
#define STREAM_H

#include <stdbool.h>

#include "record.h"

void stream_start(void);
void stream_enable(bool on);        /* also mutes the ESP-IDF log while on */
bool stream_enabled(void);
void stream_push(const record_t *r);   /* never waits */

#endif
