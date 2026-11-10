/* Daisy chain on UART1 and UART2. A valid packet that arrives on one port is repeated on the other port.
   Each board sends its own sensor packets out of both ports and saves the packets of the other boards on its
   SD card. Topology: a line (no ring). A packet with the own device ID is dropped, so a wrong ring cable
   cannot make a packet circle.

   Packet (all multi-byte values little endian):
     0xA5 0x5A | device u8 | sensor type u8 | count u8 | count records | CRC32 u32
   The CRC32 (esp_rom_crc32_le, start value 0) covers device, type, count, and the records.
   Sensor type 1 = ADS1115, type 2 = load cell (NAU7802). Both use record_t (8 bytes). A packet holds one type.
   Type 4 = calibration block (6 META records of record_t, see cal.h), sent when the values change, at the start of a
   recording, and every CAL_PERIOD_S seconds.
   Type 3 = heartbeat, one 1-byte record (CHAIN_HELLO_* flags), sent each second, so the other boards see
   this board also without sensor data.
   A packet holds 1 to 32 records; this board sends 16. */
#ifndef CHAIN_H
#define CHAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

#define CHAIN_TYPE_ADS      1
#define CHAIN_TYPE_LOADCELL 2
#define CHAIN_TYPE_HELLO    3
#define CHAIN_TYPE_CAL      4         /* calibration block: 6 META records (cal.h), 8 bytes each */

#define CHAIN_HELLO_REC     0x01U     /* recording (saving) is on */
#define CHAIN_HELLO_AUTO    0x02U     /* Mode 1 switch: auto record */
#define CHAIN_HELLO_SENSOR  0x04U     /* the ADS1115 answers */
#define CHAIN_HELLO_SD      0x08U     /* an SD card is mounted */
#define CHAIN_HELLO_LOADCELL 0x10U    /* the NAU7802 gives samples */
#define CHAIN_HELLO_WIFI    0x20U     /* the telemetry link to the server works (telem.h) */

typedef struct {
    uint32_t rx_ok[2], rx_bad[2];     /* valid packets and CRC or format errors, per port */
    uint32_t forwarded;
    uint32_t tx_drop;                 /* a TX buffer was full */
    uint32_t own_dropped;             /* packets with the own device ID */
    uint32_t tx_packets;              /* own packets */
    uint32_t tx_cal, rx_cal;          /* calibration blocks (type 4) sent, and received from other boards */
} chain_stats_t;

void chain_start(void);
/* Call for each sample, also when the board is not recording (recording only means saving). Never waits.
   One writer at a time: the sink in main.c holds its lock. */
void chain_push_ads(const record_t *r);
void chain_push_lc(const record_t *r);
/* A calibration block: n records in one packet, sent at once. */
void chain_push_cal(const record_t *r, unsigned n);
void chain_get_stats(chain_stats_t *out);
/* Bit i set = a valid packet of device ID i came in the last max_age_ms (the own ID is never set).
   flags (8 bytes, can be NULL) gets the last heartbeat flags of each ID. */
uint8_t chain_seen(uint32_t max_age_ms, uint8_t flags[8]);

#endif
