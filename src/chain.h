/* Daisy chain on UART1 and UART2. A valid packet that arrives on one port is repeated on the other port.
   Each board sends its own sensor packets out of both ports and saves the packets of the other boards on its
   SD card. Topology: a line (no ring). A packet with the own device ID is dropped, so a wrong ring cable
   cannot make a packet circle.

   Packet (all multi-byte values little endian):
     0xA5 0x5A | device u8 | sensor type u8 | count u8 | count records | CRC32 u32
   The CRC32 (esp_rom_crc32_le, start value 0) covers device, type, count, and the records.
   Sensor type 1 = ADS1115, record = record_t (8 bytes). Type 2 = load cell, 9 bytes (not made yet).
   A packet holds 1 to 32 records; this board sends 16. */
#ifndef CHAIN_H
#define CHAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

#define CHAIN_TYPE_ADS      1
#define CHAIN_TYPE_LOADCELL 2

typedef struct {
    uint32_t rx_ok[2], rx_bad[2];     /* valid packets and CRC or format errors, per port */
    uint32_t forwarded;
    uint32_t tx_drop;                 /* a TX buffer was full */
    uint32_t own_dropped;             /* packets with the own device ID */
    uint32_t tx_packets;              /* own packets */
} chain_stats_t;

void chain_start(void);
/* Call for each ADS1115 sample, also when the board is not recording (recording only means saving). Never waits. */
void chain_push_ads(const record_t *r);
void chain_get_stats(chain_stats_t *out);

#endif
