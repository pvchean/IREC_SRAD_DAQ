/* Live telemetry over WiFi (the link to the server in server/, frame format in server/telem_proto.h).

   The same firmware runs on all DAQs. The WiFi mode is stored in the flash (NVS) and read once at boot:
     OFF       (default) No WiFi code runs. No radio, no task, no buffer. telem_push() is one test of a variable.
     UPLOAD    The DAQ near the rocket. It keeps every Nth sample of each sensor and sends them to the server in batches.
     DOWNLOAD  The DAQ in the bunker. It asks the server for the data and prints it on the USB live stream, in the
               format of stream.c. It does not print its own sensors.
   A new mode starts after the next reboot. There is no switch while the DAQ runs.

   Telemetry is best effort. It never waits for the network and never blocks a sensor, the NAND, or the SD card. When the
   ring is full or the link is down, it drops the oldest data. The full data stays in the NAND and SD files. */
#ifndef TELEM_H
#define TELEM_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

typedef enum { TELEM_OFF = 0, TELEM_UPLOAD = 1, TELEM_DOWNLOAD = 2 } telem_mode_t;

/* META kinds that the DOWNLOAD DAQ puts into the stream buffer. stream.c prints them as text lines (kinds 1..6 are the calibration block).
   One batch from the server gives its records, and then these four, in this order. The AGE line is the end of the batch.
   A viewer puts the samples of the batch before the AGE line in time: the last one at the time of the line, the others
   one interval earlier each, with the interval of that sensor. */
#define TELEM_META_DTPT  10U    /* u32: time between two PT samples of the batch, in us (0 = not known) -> "T:DTPT <us>" */
#define TELEM_META_DTLC  11U    /* u32: the same for the load cell -> "T:DTLC <us>" */
#define TELEM_META_LINK  9U     /* u32: bits 15..0 drops of the upload DAQ, 23..16 RSSI of the upload DAQ (int8), 31..24 own RSSI (int8)
                                   -> "T:LINK <drops> <rssi_upload> <rssi_own>" */
#define TELEM_META_AGE   8U     /* u32: age of the newest data on the server, in ms (0xFFFFFFFF = none yet) -> "T:AGE <ms>" */

#define TELEM_DEFAULT_PORT   48613U
#define TELEM_DEFAULT_DECIM_PT  15U      /* 883 SPS / 15 = 59 Hz */
#define TELEM_DEFAULT_DECIM_LC  5U       /* 320 SPS / 5 = 64 Hz */

/* The mode of this boot. 0 (OFF) until telem_init() has read the flash. */
extern volatile uint8_t telem_run_mode;

void telem_push_slow(const record_t *r);

/* Call for each sample, from the sink of main.c (under its lock: one writer at a time). Never waits. */
static inline void telem_push(const record_t *r)
{
    if (telem_run_mode == TELEM_UPLOAD) { telem_push_slow(r); }
}

/* DOWNLOAD DAQ: the own sensors give no live output (they read zero). */
static inline bool telem_hide_local(void) { return telem_run_mode == TELEM_DOWNLOAD; }

/* Read the settings from the flash (after cal_init(), which starts the NVS). Allocates the ring in UPLOAD mode. No radio yet. */
void telem_init(void);
/* Start WiFi and the task of the mode. Call after the PFS mount. Does nothing in OFF mode. */
void telem_start(void);

/* True while the link to the server works (the server answered HELLO). */
bool telem_link_up(void);

typedef struct {
    telem_mode_t saved_mode, run_mode;
    char ssid[33], host[64];
    bool pass_set, token_set;
    uint16_t port, decim_pt, decim_lc;
    bool wifi_up, link_up;
    int rssi;
    uint32_t drops, sent_frames, reconnects;
} telem_info_t;

void telem_get_info(telem_info_t *out);
/* Settings: each call saves to the flash. They return false if the value is wrong or the flash write fails. */
bool telem_set_mode(telem_mode_t m);
bool telem_set_ssid(const char *ssid);
bool telem_set_pass(const char *pass);
bool telem_set_server(const char *host, uint16_t port, const char *token);
bool telem_set_decim(uint16_t pt, uint16_t lc);

#endif
