/* The firmware frame code (telem_frame.h, the same file that src/telem.c uses) against the server frame code (tp_util.hpp). */
#include <cstdio>
#include <vector>

#include "../tp_util.hpp"
#define TP_CRC32(p, n) tp::crc32((p), (n))
#include "../telem_frame.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what) do { if (cond) { g_pass++; } else { g_fail++; std::printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static_assert(sizeof(tp_hello_t) == 25, "HELLO payload is 25 bytes");
static_assert(sizeof(tp_get_t) == 12, "GET payload is 12 bytes");
static_assert(sizeof(tp_status_t) == 16, "STATUS payload is 16 bytes");

int main()
{
    /* CRC: the known zlib check value of "123456789" */
    const uint8_t chk[] = { '1','2','3','4','5','6','7','8','9' };
    CHECK(tp::crc32(chk, 9) == 0xCBF43926U, "crc32 check value");

    for (size_t n : { (size_t)0, (size_t)1, (size_t)8, (size_t)48, (size_t)250 * 8, (size_t)TP_MAX_PAYLOAD }) {
        std::vector<uint8_t> pay(n);
        for (size_t i = 0; i < n; i++) { pay[i] = (uint8_t)(i * 7 + 3); }

        /* firmware builds, server parses */
        std::vector<uint8_t> a(TP_HDR + n + TP_CRC);
        size_t sz = tp_make_frame(a.data(), 5, TP_DATA_LC, pay.data(), n);
        CHECK(sz == a.size(), "firmware frame size");
        size_t used = 0; tp::View v;
        CHECK(tp::parse(a.data(), a.size(), &used, &v) == tp::Parse::Frame && used == sz, "server parses firmware frame");
        CHECK(v.src == 5 && v.type == TP_DATA_LC && v.len == n && (n == 0 || memcmp(v.pay, pay.data(), n) == 0), "same fields");

        /* server builds, firmware checks; bytes are identical */
        std::vector<uint8_t> b = tp::build(5, TP_DATA_LC, pay.data(), n);
        CHECK(b == a, "both builders give the same bytes");
        uint8_t type = 0; const uint8_t *p = nullptr; size_t len = 0;
        CHECK(tp_check_frame(b.data(), b.size(), &type, &p, &len) == (int)b.size() && type == TP_DATA_LC && len == n, "firmware checks server frame");

        /* partial: need more; one flipped bit: wrong */
        CHECK(tp_check_frame(b.data(), b.size() - 1, &type, &p, &len) == 0, "partial frame: need more");
        std::vector<uint8_t> c = b;
        c[TP_HDR] ^= 0x10;                  /* the first payload byte (the first CRC byte when there is no payload) */
        CHECK(tp_check_frame(c.data(), c.size(), &type, &p, &len) == -1, "bit error: wrong");
        CHECK(tp::parse(c.data(), c.size(), &used, &v) == tp::Parse::Bad, "bit error: server says bad");
    }

    /* the header values that both sides use are one file, so check the layout once */
    uint8_t h[TP_HDR + TP_CRC];
    tp_make_frame(h, 9, TP_ACK, nullptr, 0);
    CHECK(h[0] == 0xA5 && h[1] == 0x5B && h[2] == 1 && h[3] == 9 && h[4] == TP_ACK && h[5] == 0 && h[6] == 0, "header layout");

    /* little-endian helpers */
    uint8_t q[8];
    tp_put_u64(q, 0x0102030405060708ULL);
    CHECK(q[0] == 8 && q[7] == 1 && tp_get_u64(q) == 0x0102030405060708ULL, "u64 little endian");

    std::printf("frame compatibility: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
