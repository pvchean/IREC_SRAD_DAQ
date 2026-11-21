/* ADS1115 on I2C0, continuous, 860 SPS. Two modes (saved in the flash, applied after a reset):
   - PT (default): AIN0 at +/-6.144 V, the PT stream. The thermistors are never read.
   - Thermistors only (no PT): AIN2 and AIN3 at +/-4.096 V (125 uV per count), 50 readings/s. The circuit of each is
     5VA - NTC - node - 560 ohm - GND with a filter capacitor on the node.
   Port of ADS1115_Driver.cpp. */
#ifndef ADS1115_H
#define ADS1115_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t timestamp_us;   /* time of the data-ready edge (low 32 bits, wraps in 71 min) */
    int16_t  reading;
} ads_sample_t;

typedef struct {
    uint32_t timestamp_us;
    uint16_t raw[2];         /* AIN2, AIN3: bits 14..0 = count, bit 15 = 1 for a 100 kohm thermistor (0 = 10 kohm);
                                REC_NTC_ABSENT (0xFFFF) = not installed */
} ntc_sample_t;

typedef void (*ads_cb_t)(const ads_sample_t *s);
typedef void (*ntc_cb_t)(const ntc_sample_t *s);

/* Starts the sensor, the ISR, and the task. The callbacks run in the ADS task and must not wait.
   cb gets the PT samples (PT mode). ncb gets a reading (thermistor mode) only when at least one thermistor is installed. */
int ads1115_start(ads_cb_t cb, ntc_cb_t ncb);

/* True after the chip answered and the data-ready interrupt runs. */
bool ads1115_running(void);

typedef struct {
    uint32_t scans;          /* completed cycles */
    uint32_t failed;         /* cycles stopped (a missed ready edge, an I2C error): nothing was logged, the next one starts again */
    bool     have;           /* a reading exists */
    uint32_t age_ms;         /* age of the last reading */
    ntc_sample_t last;
} ntc_stats_t;
void ads1115_ntc_stats(ntc_stats_t *out);

/* The thermistor settings, in the flash. only: no PT, thermistor readings 50 times per second (a DAQ with only thermistors);
   it applies after a reset. type[ch] (ch 0 = AIN2, 1 = AIN3): 0 = 10 kohm, 1 = 100 kohm; it applies at once. */
typedef struct { bool only; uint8_t type[2]; } ntc_cfg_t;
void ads1115_ntc_cfg(ntc_cfg_t *saved, bool *running_only);
bool ads1115_ntc_set_only(bool only);                    /* false: the flash write failed */
bool ads1115_ntc_set_type(int ch, int type);
bool ads1115_only_running(void);                         /* the running mode is thermistors only */

#endif
