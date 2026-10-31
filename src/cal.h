/* Calibration values of the two sensors: A and B of each. MATLAB sends them (USB command CAL), the DAQ keeps them in its
   flash (NVS), and writes them into the recorded data as a block of 6 META records (record.h): in the PFS file, on the SD
   card, and to the other boards of the chain. The block is written again at each start of the recording, after PFSFLUSH,
   after each CAL command, and every CAL_PERIOD_S seconds, so each file (also a file that the ring cut at its start) has one.
   PT: pressure (PSI) = A x V + B, with V in volts. Load cell: force (N) = A x (mV/V) + B. */
#ifndef CAL_H
#define CAL_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

#define CAL_PT          0
#define CAL_LC          1
#define CAL_FLAG_PT     0x01U          /* the PT has a saved calibration */
#define CAL_FLAG_LC     0x02U
#define CAL_BLOCK_RECS  6U
#define CAL_PERIOD_S    30U

typedef struct {
    float    a[2], b[2];               /* index CAL_PT, CAL_LC */
    uint32_t rev;                      /* 24 bits: counts each CAL command */
    uint8_t  flags;
} cal_t;

/* The state of the block parser (cal_parse). */
typedef struct {
    unsigned n;
    record_t r[CAL_BLOCK_RECS];
} cal_parser_t;

void cal_init(void);                                       /* loads the values from the flash; call once, early */
bool cal_flash_ok(void);                                   /* false: the flash store does not work, the values are lost at a reset */
void cal_get(cal_t *out);
/* Sets A and B of one sensor, counts the revision, writes the flash, and asks for a new block in the data.
   Returns false if the flash write failed (the values stay in RAM until the next reset). */
bool cal_set(int sensor, float a, float b, cal_t *out);

void cal_request(void);                                    /* a new block before the next record (a new PFS file) */
bool cal_take_request(void);                               /* true one time after cal_request() or cal_set() */
void cal_make_block(record_t out[CAL_BLOCK_RECS]);

/* Feed each record of a file in order. True when the record completed a valid block (the six kinds in order, with the
   right CRC); the values are in *out (can be NULL). Any other record resets the parser. */
bool cal_parse(cal_parser_t *p, const record_t *r, cal_t *out);

#endif
