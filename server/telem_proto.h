/* The wire format of the telemetry link. The DAQ firmware (C) and the server (C++) both include this file.
   All numbers are little endian. One TCP connection carries these frames:

     A5 5B | ver | src_dev | type | len (u16) | payload (len bytes) | CRC32 (u32)

   The CRC32 is the standard zlib CRC32 (the same as esp_rom_crc32_le(0, ...)). It covers ver, src_dev, type, len and the payload.
   The data records are the 8-byte record_t of src/record.h, not changed. They keep the time and seq of the board. */
#ifndef TELEM_PROTO_H
#define TELEM_PROTO_H

#include <stdint.h>

#define TP_SYNC0        0xA5U
#define TP_SYNC1        0x5BU
#define TP_VER          1U
#define TP_HDR          7U              /* sync 2, ver, src_dev, type, len 2 */
#define TP_CRC          4U
#define TP_MAX_PAYLOAD  16000U
#define TP_MAX_UPLOAD   2048U           /* the largest payload of an upload frame (the server refuses more) */
#define TP_REC_SIZE     8U             /* sizeof(record_t) */
#define TP_TOKEN_LEN    16U
#define TP_CAL_RECS     6U              /* the calibration block: 6 META records (src/cal.h) */

/* Frame types. */
#define TP_HELLO        1U              /* client -> server. payload: role u8, token[16], cursor u64 (tp_hello_t) */
#define TP_ACK          2U              /* server -> client. payload: head cursor u64 (the next index the server will give) */
#define TP_DATA_PT      3U              /* upload. payload: N records of the pressure sensor */
#define TP_DATA_LC      4U              /* upload. payload: N records of the load cell */
#define TP_CAL          5U              /* upload. payload: 6 META records (the calibration block) */
#define TP_STATUS       6U              /* upload. payload: tp_status_t */
#define TP_GET          7U              /* download. payload: cursor u64, max_bytes u16, wait_ms u16 (tp_get_t) */
#define TP_BATCH        8U              /* server -> download client. payload: next_cursor u64, age_ms u32, then whole stored frames */
#define TP_NAK          9U              /* server -> client. payload: reason u8. The server closes the connection after it. */
#define TP_RESET        10U             /* download -> server, no payload: close the archive files, start new ones, empty the ring.
                                           server -> download: head cursor u64, then the name of the new archive file (text). */

#define TP_ROLE_UPLOAD    1U
#define TP_ROLE_DOWNLOAD  2U

#define TP_NAK_TOKEN    1U
#define TP_NAK_ROLE     2U
#define TP_NAK_FRAME    3U

/* The cursor of a download client. 0 = from the oldest frame the server has. TP_CURSOR_NOW = only new frames (and the last CAL). */
#define TP_CURSOR_NOW   0xFFFFFFFFFFFFFFFFULL
#define TP_AGE_NONE     0xFFFFFFFFU     /* in a BATCH: the server has no data yet */

#ifdef __GNUC__
#define TP_PACKED __attribute__((packed))
#else
#define TP_PACKED
#endif

typedef struct TP_PACKED { uint8_t role; uint8_t token[TP_TOKEN_LEN]; uint64_t cursor; } tp_hello_t;
typedef struct TP_PACKED { uint64_t cursor; uint16_t max_bytes; uint16_t wait_ms; } tp_get_t;
/* The state of the upload DAQ. Sent about every 2 s. rssi is in dBm (0 = not known). drops counts the samples that the
   telemetry ring could not take (the full data is still in the NAND and SD files). */
typedef struct TP_PACKED { int8_t rssi; uint8_t flags; uint32_t drops; uint32_t queue; uint32_t uptime_s; uint16_t reconnects; } tp_status_t;

#endif
