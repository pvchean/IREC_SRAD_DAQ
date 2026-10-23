/* The record on NAND: 8 bytes, little endian. The seq field counts all samples, also the samples that
   are not recorded, so a gap in seq shows a pause of the recording or a lost sample. */
#ifndef RECORD_H
#define RECORD_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t timestamp_us;
    int16_t  reading;
    uint16_t seq;
} record_t;

#endif
