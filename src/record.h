/* The record: 8 bytes, little endian, in the PFS file, in the SD file, and in the chain packets. One stream holds
   both sensors, so the size must stay the same: a PFS file finds its first whole record by rounding to 8 bytes.

     byte        PT (ADS1115)               load cell (NAU7802)
     0..3        u32 time of the RDY edge   u32 time of the DRDY edge
     4..5        i16 raw count              i24 raw count (bytes 4..6)
     6..7        bit 15 = 0, bits 14..0 seq byte 7: bit 7 = 1, bits 6..0 seq

   A file of the firmware before the load cell uses all 16 bits for the seq, and all its records are PT records.
   Its top seq bit changes only once in 32768 records. In a file with a load cell, that bit changes all the time.

   Each sensor has its own seq. It counts all samples, also the samples that are not recorded, so a gap in seq shows a
   pause of the recording or a lost sample. The PT seq wraps in 37 s, the load cell seq in 0.4 s: the time shows longer gaps.

   A META record is not a sample. Its time field (bytes 0..3) is in 0xFFFFFFF0..0xFFFFFFFF: kind = time - 0xFFFFFFF0. Bytes 4..7
   are the payload. A sensor time is never in that range (rec_ts() keeps it below, 16 us less once in 71 min), so a file of the
   older firmware (no META) still decodes. The calibration block is 6 META records, one after another (see cal.h):
     kind 1 BEGIN   u32: bits 31..24 flags (1 = PT calibrated, 2 = load cell calibrated), bits 23..0 revision
     kind 2..5      float32: PT A, PT B, load cell A, load cell B
     kind 6 END     u32: zlib CRC-32 over the 20 payload bytes of kinds 1 to 5
   PT: pressure (PSI) = A x V + B (V in volts). Load cell: force (N) = A x (mV/V) + B. A reader ignores a block that is not
   complete or whose CRC is wrong (a file that the ring cut at its start). */
#ifndef RECORD_H
#define RECORD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t timestamp_us;
    int16_t  reading;
    uint16_t seq;
} record_t;

#define REC_LC_FLAG      0x8000U       /* in seq: a load cell record */
#define REC_PT_SEQ_MASK  0x7FFFU
#define REC_LC_SEQ_MASK  0x7FU

#define REC_META_BASE    0xFFFFFFF0U   /* the time field of a META record: REC_META_BASE + kind */

static inline bool rec_is_meta(const record_t *r) { return r->timestamp_us >= REC_META_BASE; }

/* The time of a sensor record. It never gets the value of a META record. */
static inline uint32_t rec_ts(uint32_t us) { return us >= REC_META_BASE ? REC_META_BASE - 1U : us; }

static inline bool rec_is_lc(const record_t *r) { return !rec_is_meta(r) && (r->seq & REC_LC_FLAG) != 0U; }

static inline uint32_t rec_meta_payload(const record_t *r) { return (uint32_t)(uint16_t)r->reading | ((uint32_t)r->seq << 16); }

static inline record_t rec_make_meta(uint32_t kind, uint32_t payload)
{
    record_t r = { REC_META_BASE + kind, (int16_t)(uint16_t)payload, (uint16_t)(payload >> 16) };
    return r;
}

static inline record_t rec_make_lc(uint32_t timestamp_us, int32_t count, uint8_t seq)
{
    uint32_t c = (uint32_t)count & 0xFFFFFFU;
    record_t r = { timestamp_us, (int16_t)(uint16_t)c,
                   (uint16_t)((c >> 16) | REC_LC_FLAG | ((uint32_t)(seq & REC_LC_SEQ_MASK) << 8)) };
    return r;
}

static inline int32_t rec_lc_count(const record_t *r)
{
    uint32_t c = (uint32_t)(uint16_t)r->reading | ((uint32_t)(r->seq & 0xFFU) << 16);
    return (int32_t)(c << 8) >> 8;
}

static inline uint8_t rec_lc_seq(const record_t *r) { return (uint8_t)((r->seq >> 8) & REC_LC_SEQ_MASK); }

#endif
