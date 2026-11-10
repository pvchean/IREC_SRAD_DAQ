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
/* Waits up to timeout_ms for room. Only for a task that is not a sensor task (the DOWNLOAD DAQ, telem.c); then it is the only writer.
   META records are printed as text lines: the calibration block as "CALRX:OK rev=..", the link state as "T:DTPT <us>", "T:DTLC <us>",
   "T:LINK <drops> <rssi_upload> <rssi_own>", and "T:AGE <ms>" (the last line of a batch from the server). See telem.h. */
bool stream_push_wait(const record_t *r, uint32_t timeout_ms);

#endif
