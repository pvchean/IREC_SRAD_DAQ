/* NAU7802 on I2C1: load cell bridge on channel 1, gain 128, 320 SPS, internal LDO at 4.5 V. The load cell is optional:
   a board with no NAU7802 tries again each second and works as before. */
#ifndef NAU7802_H
#define NAU7802_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t timestamp_us;   /* time of the data-ready edge (low 32 bits, wraps in 71 min) */
    int32_t  reading;        /* raw 24-bit count, sign extended */
} nau_sample_t;

typedef struct {
    uint32_t samples;        /* samples given to the callback */
    uint32_t i2c_errors;     /* failed reads */
    uint32_t backup_reads;   /* samples with no ready edge: the time is the time of the read */
    uint32_t restarts;       /* the chip stopped and the driver set it up again */
    uint32_t ocal;           /* offset calibration register: bit 23 = sign, bits 22..0 = size in counts */
} nau_stats_t;

typedef void (*nau_cb_t)(const nau_sample_t *s);

/* Starts the bus and the task. The callback runs in the NAU task and must not wait. */
int nau7802_start(nau_cb_t cb);

/* True while the chip gives samples. */
bool nau7802_running(void);

/* True if the chip gave samples and then stopped. */
bool nau7802_fault(void);

void nau7802_get_stats(nau_stats_t *out);

#endif
