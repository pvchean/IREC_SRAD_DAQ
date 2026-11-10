/* Frame helpers for the server and its test client (C++ only). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "telem_proto.h"

namespace tp {

inline uint32_t crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) { c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1); }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < n; i++) { c = table[(c ^ p[i]) & 0xFFU] ^ (c >> 8); }
    return c ^ 0xFFFFFFFFU;
}

inline void put16(std::vector<uint8_t> &v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
inline void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) { v.push_back((uint8_t)(x >> (8 * i))); } }
inline void put64(std::vector<uint8_t> &v, uint64_t x) { for (int i = 0; i < 8; i++) { v.push_back((uint8_t)(x >> (8 * i))); } }
inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

/* One complete frame, with the CRC. */
inline std::vector<uint8_t> build(uint8_t src, uint8_t type, const uint8_t *pay, size_t n)
{
    std::vector<uint8_t> f;
    f.reserve(TP_HDR + n + TP_CRC);
    f.push_back(TP_SYNC0); f.push_back(TP_SYNC1); f.push_back(TP_VER); f.push_back(src); f.push_back(type);
    put16(f, (uint16_t)n);
    if (n) { f.insert(f.end(), pay, pay + n); }
    put32(f, crc32(f.data() + 2, f.size() - 2));
    return f;
}

enum class Parse { Need, Frame, Bad };

/* Look at buf (n bytes). Frame: a valid frame is at buf; *used is its size. Bad: skip *used bytes (junk or a bad CRC).
   Need: wait for more bytes. */
struct View { uint8_t src = 0, type = 0; const uint8_t *pay = nullptr; size_t len = 0; };

inline Parse parse(const uint8_t *buf, size_t n, size_t *used, View *v)
{
    if (n < 2) { *used = 0; return Parse::Need; }
    if (buf[0] != TP_SYNC0 || buf[1] != TP_SYNC1) { *used = 1; return Parse::Bad; }
    if (n < TP_HDR) { *used = 0; return Parse::Need; }
    size_t len = get16(buf + 5);
    if (buf[2] != TP_VER || len > TP_MAX_PAYLOAD) { *used = 1; return Parse::Bad; }
    size_t total = TP_HDR + len + TP_CRC;
    if (n < total) { *used = 0; return Parse::Need; }
    if (get32(buf + TP_HDR + len) != crc32(buf + 2, TP_HDR - 2 + len)) { *used = 1; return Parse::Bad; }
    *used = total;
    if (v) { v->src = buf[3]; v->type = buf[4]; v->pay = buf + TP_HDR; v->len = len; }
    return Parse::Frame;
}

}  // namespace tp
