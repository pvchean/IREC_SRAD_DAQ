/* Build and check one telemetry frame (format: telem_proto.h). Plain C, no OS calls, so the DAQ firmware and the host tests use the
   same code. The file that includes it must define TP_CRC32(ptr, len): the zlib CRC32 of len bytes (esp_rom_crc32_le(0, ptr, len) on the DAQ). */
#ifndef TELEM_FRAME_H
#define TELEM_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "telem_proto.h"

#ifndef TP_CRC32
#error "define TP_CRC32(ptr, len) before including telem_frame.h"
#endif

static inline void tp_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void tp_put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); } }
static inline void tp_put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (8 * i)); } }
static inline uint16_t tp_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t tp_get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t tp_get_u64(const uint8_t *p) { return (uint64_t)tp_get_u32(p) | ((uint64_t)tp_get_u32(p + 4) << 32); }

/* Write a whole frame at buf (payload copied from pay, if pay is not NULL) and return its size: TP_HDR + n + TP_CRC bytes. */
static inline size_t tp_make_frame(uint8_t *buf, uint8_t src, uint8_t type, const void *pay, size_t n)
{
    buf[0] = TP_SYNC0; buf[1] = TP_SYNC1; buf[2] = TP_VER; buf[3] = src; buf[4] = type;
    tp_put_u16(buf + 5, (uint16_t)n);
    if (pay && n) { memcpy(buf + TP_HDR, pay, n); }
    tp_put_u32(buf + TP_HDR + n, TP_CRC32(buf + 2, (uint32_t)(TP_HDR - 2U + n)));
    return TP_HDR + n + TP_CRC;
}

/* Check the frame at p (n bytes available). Returns its size, 0 if more bytes are needed, -1 if it is wrong (sync, version, length, CRC). */
static inline int tp_check_frame(const uint8_t *p, size_t n, uint8_t *type, const uint8_t **pay, size_t *len)
{
    if (n < TP_HDR) { return 0; }
    if (p[0] != TP_SYNC0 || p[1] != TP_SYNC1 || p[2] != TP_VER) { return -1; }
    size_t l = tp_get_u16(p + 5);
    if (l > TP_MAX_PAYLOAD) { return -1; }
    if (n < TP_HDR + l + TP_CRC) { return 0; }
    if (tp_get_u32(p + TP_HDR + l) != TP_CRC32(p + 2, (uint32_t)(TP_HDR - 2U + l))) { return -1; }
    *type = p[4]; *pay = p + TP_HDR; *len = l;
    return (int)(TP_HDR + l + TP_CRC);
}

#endif
