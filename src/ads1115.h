/* ADS1115 on I2C0: continuous 860 SPS, AIN0, +/-6.144 V. Port of ADS1115_Driver.cpp. */
#ifndef ADS1115_H
#define ADS1115_H

#include <stdint.h>

typedef struct {
    uint32_t timestamp_us;   /* time of the data-ready edge (low 32 bits, wraps in 71 min) */
    int16_t  reading;
} ads_sample_t;

typedef void (*ads_cb_t)(const ads_sample_t *s);

/* Starts the sensor, the ISR, and the task. The callback runs in the ADS task and must not wait. */
int ads1115_start(ads_cb_t cb);

#endif
