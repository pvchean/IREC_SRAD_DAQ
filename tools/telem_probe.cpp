/* Probe a running telemetry server over the network (Claude's helper, not a deliverable).
   Build: g++ -O2 -std=c++17 -I../server -o telem_probe telem_probe.cpp
   Modes:
     telem_probe nak  HOST PORT          HELLO with a wrong token. A good server answers NAK(1) and closes. Changes nothing.
     telem_probe down HOST PORT TOKEN    Download HELLO + one GET (cursor NOW, wait 3 s). Read only: shows head, age, frames.
     telem_probe trip HOST PORT TOKEN    Round trip: uploads ONE DATA_PT frame (src_dev 0xEE, 2 records, reading 0), then
                                         downloads it and compares the bytes. This frame stays in the server ring and archive. */
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "tp_util.hpp"

static int64_t ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Conn {
    int fd = -1;
    std::vector<uint8_t> rx;
    bool open(const char *host, const char *port)
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, port, &hints, &res) != 0 || !res) { std::printf("DNS: cannot resolve %s\n", host); return false; }
        for (addrinfo *a = res; a; a = a->ai_next) {
            fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd >= 0 && ::connect(fd, a->ai_addr, a->ai_addrlen) == 0) { freeaddrinfo(res); return true; }
            if (fd >= 0) { ::close(fd); fd = -1; }
        }
        freeaddrinfo(res);
        std::printf("TCP: cannot connect to %s:%s\n", host, port);
        return false;
    }
    void send_frame(uint8_t src, uint8_t type, const std::vector<uint8_t> &pay)
    {
        std::vector<uint8_t> f = tp::build(src, type, pay.data(), pay.size());
        ::send(fd, f.data(), f.size(), MSG_NOSIGNAL);
    }
    /* 1 = frame, 0 = timeout, -1 = closed */
    int recv_frame(int timeout_ms, uint8_t *type, std::vector<uint8_t> *pay, size_t *junk)
    {
        int64_t end = ms() + timeout_ms;
        for (;;) {
            size_t used; tp::View v;
            tp::Parse r = tp::parse(rx.data(), rx.size(), &used, &v);
            if (r == tp::Parse::Frame) { *type = v.type; pay->assign(v.pay, v.pay + v.len); rx.erase(rx.begin(), rx.begin() + (long)used); return 1; }
            if (r == tp::Parse::Bad) { *junk += used; rx.erase(rx.begin(), rx.begin() + (long)used); continue; }
            int64_t left = end - ms();
            if (left <= 0) { return 0; }
            pollfd p{ fd, POLLIN, 0 };
            if (::poll(&p, 1, (int)left) > 0) {
                uint8_t buf[16384];
                ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n <= 0) { return -1; }
                rx.insert(rx.end(), buf, buf + n);
            }
        }
    }
    ~Conn() { if (fd >= 0) { ::close(fd); } }
};

static std::vector<uint8_t> hello(uint8_t role, const std::string &tok, uint64_t cursor)
{
    std::vector<uint8_t> p{ role };
    for (size_t i = 0; i < TP_TOKEN_LEN; i++) { p.push_back(i < tok.size() ? (uint8_t)tok[i] : 0); }
    tp::put64(p, cursor);
    return p;
}

static std::vector<uint8_t> get(uint64_t cursor, uint16_t max_bytes, uint16_t wait_ms)
{
    std::vector<uint8_t> p;
    tp::put64(p, cursor); tp::put16(p, max_bytes); tp::put16(p, wait_ms);
    return p;
}

static const char *tname(uint8_t t)
{
    static const char *n[] = { "?", "HELLO", "ACK", "DATA_PT", "DATA_LC", "CAL", "STATUS", "GET", "BATCH", "NAK" };
    return t < 10 ? n[t] : "?";
}

/* HELLO, expect ACK. Returns the head cursor, or UINT64_MAX on failure. */
static uint64_t login(Conn &c, uint8_t role, const std::string &tok)
{
    uint8_t t; std::vector<uint8_t> p; size_t junk = 0;
    int64_t t0 = ms();
    c.send_frame(0xEE, TP_HELLO, hello(role, tok, 0));
    int r = c.recv_frame(5000, &t, &p, &junk);
    if (r != 1) { std::printf("HELLO: no answer (%s, %zu junk bytes)\n", r == 0 ? "timeout" : "closed", junk); return UINT64_MAX; }
    if (t == TP_NAK) { std::printf("HELLO: NAK reason %u (%s)\n", p.empty() ? 0 : p[0], !p.empty() && p[0] == TP_NAK_TOKEN ? "wrong token" : "other"); return UINT64_MAX; }
    if (t != TP_ACK || p.size() != 8) { std::printf("HELLO: unexpected %s len %zu\n", tname(t), p.size()); return UINT64_MAX; }
    uint64_t head = tp::get64(p.data());
    std::printf("HELLO: ACK as %s in %lld ms, head cursor %llu\n", role == TP_ROLE_UPLOAD ? "UPLOAD" : "DOWNLOAD", (long long)(ms() - t0), (unsigned long long)head);
    return head;
}

/* One GET; prints the BATCH. Returns the frames in it (each as its full bytes). */
static bool one_get(Conn &c, uint64_t cursor, uint16_t wait_ms, std::vector<std::vector<uint8_t>> *frames)
{
    uint8_t t; std::vector<uint8_t> p; size_t junk = 0;
    int64_t t0 = ms();
    c.send_frame(0xEE, TP_GET, get(cursor, 4096, wait_ms));
    int r = c.recv_frame(wait_ms + 5000, &t, &p, &junk);
    if (r != 1 || t != TP_BATCH || p.size() < 12) { std::printf("GET: no BATCH (r=%d type=%s)\n", r, r == 1 ? tname(t) : "-"); return false; }
    uint64_t next = tp::get64(p.data());
    uint32_t age = tp::get32(p.data() + 8);
    std::printf("GET: BATCH in %lld ms, next cursor %llu, age %s", (long long)(ms() - t0), (unsigned long long)next, age == TP_AGE_NONE ? "none (no data yet)" : "");
    if (age != TP_AGE_NONE) { std::printf("%u ms", age); }
    size_t pos = 12, n = 0;
    int count[10] = { 0 };
    while (pos < p.size()) {
        size_t used; tp::View v;
        if (tp::parse(p.data() + pos, p.size() - pos, &used, &v) != tp::Parse::Frame) { std::printf(", BAD inner frame at %zu", pos); break; }
        if (v.type < 10) { count[v.type]++; }
        if (frames) { frames->emplace_back(p.begin() + (long)pos, p.begin() + (long)(pos + used)); }
        pos += used; n++;
    }
    std::printf(", %zu frames", n);
    for (int i = 0; i < 10; i++) { if (count[i]) { std::printf(" %s=%d", tname((uint8_t)i), count[i]); } }
    std::printf("\n");
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: telem_probe nak|down|trip HOST PORT [TOKEN]\n"); return 2; }
    std::string mode = argv[1];
    const char *host = argv[2], *port = argv[3];
    std::string tok = argc > 4 ? argv[4] : "";

    if (mode == "nak") {
        Conn c;
        if (!c.open(host, port)) { return 1; }
        uint8_t t; std::vector<uint8_t> p; size_t junk = 0;
        int64_t t0 = ms();
        c.send_frame(0xEE, TP_HELLO, hello(TP_ROLE_DOWNLOAD, "x-probe-wrong-x", 0));
        int r = c.recv_frame(5000, &t, &p, &junk);
        if (r != 1) { std::printf("FAIL: no frame back (%s, %zu junk bytes). The port is open but no telemetry server answers.\n", r == 0 ? "timeout" : "closed", junk); return 1; }
        if (t == TP_NAK && p.size() == 1 && p[0] == TP_NAK_TOKEN) {
            uint8_t t2; std::vector<uint8_t> p2;
            int closed = c.recv_frame(3000, &t2, &p2, &junk);
            std::printf("OK: NAK(wrong token) in %lld ms, server %s the connection\n", (long long)(ms() - t0), closed == -1 ? "closed" : "did NOT close");
            return closed == -1 ? 0 : 1;
        }
        std::printf("FAIL: unexpected %s len %zu\n", tname(t), p.size());
        return 1;
    }
    if (tok.empty()) { std::fprintf(stderr, "this mode needs TOKEN\n"); return 2; }
    if (mode == "down") {
        Conn c;
        if (!c.open(host, port)) { return 1; }
        if (login(c, TP_ROLE_DOWNLOAD, tok) == UINT64_MAX) { return 1; }
        return one_get(c, TP_CURSOR_NOW, 3000, nullptr) ? 0 : 1;
    }
    if (mode == "trip") {
        Conn d, u;
        if (!d.open(host, port)) { return 1; }
        uint64_t head = login(d, TP_ROLE_DOWNLOAD, tok);
        if (head == UINT64_MAX) { return 1; }
        if (!u.open(host, port)) { return 1; }
        if (login(u, TP_ROLE_UPLOAD, tok) == UINT64_MAX) { return 1; }
        std::vector<uint8_t> pay;
        for (uint16_t i = 0; i < 2; i++) { tp::put32(pay, 1000U * i); tp::put16(pay, 0); tp::put16(pay, i); }  /* record_t: time, reading 0, seq */
        std::vector<uint8_t> sent = tp::build(0xEE, TP_DATA_PT, pay.data(), pay.size());
        int64_t t0 = ms();
        u.send_frame(0xEE, TP_DATA_PT, pay);
        std::vector<std::vector<uint8_t>> got;
        if (!one_get(d, head, 5000, &got)) { return 1; }
        for (const auto &f : got) {
            if (f == sent) { std::printf("OK: round trip upload -> server -> download in %lld ms, bytes identical\n", (long long)(ms() - t0)); return 0; }
        }
        std::printf("FAIL: the uploaded frame did not come back\n");
        return 1;
    }
    std::fprintf(stderr, "unknown mode\n");
    return 2;
}
