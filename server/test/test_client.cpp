/* Loopback tests of the telemetry server. A fake upload DAQ and fake download DAQs talk to a running server.
   Usage: test_client PORT TOKEN */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../tp_util.hpp"

static int g_port;
static std::string g_token;
static int g_fail = 0, g_pass = 0;

#define CHECK(cond, what) do { if (cond) { g_pass++; } else { g_fail++; std::printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static int64_t ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Conn {
    int fd = -1;
    std::vector<uint8_t> rx;
    bool open_to(int port)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (int i = 0; i < 50; i++) {                          /* the server may still start */
            if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }
    void send_raw(const std::vector<uint8_t> &b) { ::send(fd, b.data(), b.size(), MSG_NOSIGNAL); }
    void send_frame(uint8_t src, uint8_t type, const std::vector<uint8_t> &pay) { send_raw(tp::build(src, type, pay.data(), pay.size())); }
    /* Wait up to timeout_ms for one valid frame. Returns false for timeout or close. */
    bool recv_frame(int timeout_ms, uint8_t *type, std::vector<uint8_t> *pay, bool *closed = nullptr)
    {
        int64_t end = ms() + timeout_ms;
        for (;;) {
            size_t used; tp::View v;
            tp::Parse r = tp::parse(rx.data(), rx.size(), &used, &v);
            if (r == tp::Parse::Frame) {
                *type = v.type; pay->assign(v.pay, v.pay + v.len);
                rx.erase(rx.begin(), rx.begin() + (long)used);
                return true;
            }
            if (r == tp::Parse::Bad) { rx.erase(rx.begin(), rx.begin() + (long)used); continue; }
            int64_t left = end - ms();
            if (left <= 0) { return false; }
            pollfd p{ fd, POLLIN, 0 };
            if (::poll(&p, 1, (int)left) > 0) {
                uint8_t buf[8192];
                ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n <= 0) { if (closed) { *closed = true; } return false; }
                rx.insert(rx.end(), buf, buf + n);
            }
        }
    }
    ~Conn() { if (fd >= 0) { ::close(fd); } }
};

static std::vector<uint8_t> hello(uint8_t role, const std::string &tok, uint64_t cursor)
{
    std::vector<uint8_t> p;
    p.push_back(role);
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

/* n records; the time field of record i is t0+i and the reading is i. */
static std::vector<uint8_t> records(uint32_t t0, int n)
{
    std::vector<uint8_t> p;
    for (int i = 0; i < n; i++) { tp::put32(p, t0 + (uint32_t)i); tp::put16(p, (uint16_t)i); tp::put16(p, (uint16_t)i); }
    return p;
}

static bool login(Conn &c, uint8_t role, const std::string &tok, uint64_t cursor = 0, uint64_t *head = nullptr)
{
    if (!c.open_to(g_port)) { return false; }
    c.send_frame(1, TP_HELLO, hello(role, tok, cursor));
    uint8_t t; std::vector<uint8_t> p;
    if (!c.recv_frame(2000, &t, &p) || t != TP_ACK || p.size() != 8) { return false; }
    if (head) { *head = tp::get64(p.data()); }
    return true;
}

/* The nested frames of a BATCH. */
struct Batch { uint64_t next = 0; uint32_t age = 0; std::vector<std::pair<uint8_t, std::vector<uint8_t>>> frames; };

static bool read_batch(Conn &c, int timeout_ms, Batch *b)
{
    uint8_t t; std::vector<uint8_t> p;
    if (!c.recv_frame(timeout_ms, &t, &p) || t != TP_BATCH || p.size() < 12) { return false; }
    b->next = tp::get64(p.data()); b->age = tp::get32(p.data() + 8);
    b->frames.clear();
    size_t pos = 12;
    while (pos < p.size()) {
        size_t used; tp::View v;
        if (tp::parse(p.data() + pos, p.size() - pos, &used, &v) != tp::Parse::Frame) { return false; }
        b->frames.push_back({ v.type, std::vector<uint8_t>(v.pay, v.pay + v.len) });
        pos += used;
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3) { std::printf("usage: test_client PORT TOKEN\n"); return 2; }
    g_port = std::atoi(argv[1]);
    g_token = argv[2];

    /* 1. A wrong token gets a NAK and a close. */
    {
        Conn c;
        CHECK(c.open_to(g_port), "connect");
        c.send_frame(1, TP_HELLO, hello(TP_ROLE_UPLOAD, "wrong", 0));
        uint8_t t = 0; std::vector<uint8_t> p; bool closed = false;
        CHECK(c.recv_frame(2000, &t, &p) && t == TP_NAK && p.size() == 1 && p[0] == TP_NAK_TOKEN, "bad token: NAK");
        CHECK(!c.recv_frame(1000, &t, &p, &closed) && closed, "bad token: closed");
    }
    /* 2. Data before HELLO is refused. */
    {
        Conn c;
        CHECK(c.open_to(g_port), "connect");
        c.send_frame(1, TP_DATA_PT, records(1000, 3));
        uint8_t t = 0; std::vector<uint8_t> p;
        CHECK(c.recv_frame(2000, &t, &p) && t == TP_NAK && p[0] == TP_NAK_FRAME, "no HELLO: NAK");
    }

    Conn up;
    uint64_t head0 = 0;
    CHECK(login(up, TP_ROLE_UPLOAD, g_token, 0, &head0), "upload login");
    CHECK(head0 == 0, "empty server: head 0");

    /* 3. Upload: a good frame, a frame with a bad CRC (ignored, link stays), junk bytes, and a good frame again. */
    up.send_frame(1, TP_DATA_PT, records(1000, 30));
    auto bad = tp::build(1, TP_DATA_PT, records(2000, 30).data(), 240);
    bad[20] ^= 0xFF;
    up.send_raw(bad);
    up.send_raw({ 0x01, 0x02, 0xA5, 0x00, 0x77 });
    up.send_frame(1, TP_DATA_LC, records(3000, 10));
    up.send_frame(1, TP_CAL, records(0xFFFFFFF0U, 6));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));     /* let the server read the upload socket */

    /* 4. Download from the start: it gets the three good frames in order, with the original bytes. */
    Conn dn;
    CHECK(login(dn, TP_ROLE_DOWNLOAD, g_token), "download login");
    dn.send_frame(2, TP_GET, get(0, 8192, 1000));
    Batch b;
    CHECK(read_batch(dn, 3000, &b), "batch 1");
    CHECK(b.frames.size() == 3, "3 good frames (bad CRC dropped)");
    if (b.frames.size() == 3) {
        CHECK(b.frames[0].first == TP_DATA_PT && b.frames[0].second == records(1000, 30), "frame 1 is the PT data, unchanged");
        CHECK(b.frames[1].first == TP_DATA_LC && b.frames[1].second == records(3000, 10), "frame 2 is the LC data");
        CHECK(b.frames[2].first == TP_CAL, "frame 3 is CAL");
    }
    CHECK(b.next == 3, "next cursor 3");
    CHECK(b.age != TP_AGE_NONE && b.age < 2000, "age is small");

    /* 5. Long poll with no data: the empty answer comes after the wait time, not at once. */
    int64_t t0 = ms();
    dn.send_frame(2, TP_GET, get(b.next, 8192, 800));
    CHECK(read_batch(dn, 3000, &b), "empty batch");
    int64_t dt = ms() - t0;
    CHECK(b.frames.empty() && b.next == 3, "empty batch has no frames");
    CHECK(dt >= 700 && dt < 1500, "long poll waited about 800 ms");

    /* 6. Long poll wakes at once when data arrives. */
    dn.send_frame(2, TP_GET, get(b.next, 8192, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    t0 = ms();
    up.send_frame(1, TP_DATA_PT, records(4000, 30));
    CHECK(read_batch(dn, 3000, &b), "woken batch");
    CHECK(ms() - t0 < 1000, "woken within 1 s");
    CHECK(b.frames.size() == 1 && b.frames[0].second == records(4000, 30) && b.next == 4, "woken batch holds the new frame");

    /* 7. Resume: a new download connection with the old cursor gets only newer frames. */
    up.send_frame(1, TP_DATA_PT, records(5000, 30));
    {
        Conn d2;
        CHECK(login(d2, TP_ROLE_DOWNLOAD, g_token), "download 2 login");
        d2.send_frame(2, TP_GET, get(4, 8192, 1000));
        Batch b2;
        CHECK(read_batch(d2, 3000, &b2), "resume batch");
        CHECK(b2.frames.size() == 1 && b2.frames[0].second == records(5000, 30) && b2.next == 5, "resume: only the newer frame");
    }
    /* 8. A cursor from the future (the server was restarted) is accepted and gives data from the start of the ring. */
    {
        Conn d3;
        CHECK(login(d3, TP_ROLE_DOWNLOAD, g_token), "download 3 login");
        d3.send_frame(2, TP_GET, get(999999, 15000, 1000));
        Batch b3;
        CHECK(read_batch(d3, 3000, &b3), "future cursor batch");
        CHECK(b3.next <= 5, "future cursor does not run away");
    }
    /* 9. Cursor NOW: no old data, but the last CAL first. */
    {
        Conn d4;
        CHECK(login(d4, TP_ROLE_DOWNLOAD, g_token), "download 4 login");
        d4.send_frame(2, TP_GET, get(TP_CURSOR_NOW, 8192, 1000));
        Batch b4;
        CHECK(read_batch(d4, 3000, &b4), "NOW batch");
        CHECK(b4.frames.size() == 1 && b4.frames[0].first == TP_CAL, "NOW: the last CAL only");
        CHECK(b4.next == 5, "NOW: cursor at the head");
    }
    /* 10. Two download clients get the same new data. */
    {
        Conn da, db;
        CHECK(login(da, TP_ROLE_DOWNLOAD, g_token) && login(db, TP_ROLE_DOWNLOAD, g_token), "two download clients");
        da.send_frame(2, TP_GET, get(5, 8192, 3000));
        db.send_frame(2, TP_GET, get(5, 8192, 3000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        up.send_frame(1, TP_DATA_LC, records(6000, 10));
        Batch ba, bb;
        CHECK(read_batch(da, 3000, &ba) && read_batch(db, 3000, &bb), "both batches");
        CHECK(ba.frames.size() == 1 && bb.frames.size() == 1 && ba.frames[0].second == bb.frames[0].second, "same data to both");
    }
    /* 11. max_bytes limits a batch; the next GET continues. */
    {
        Conn d5;
        CHECK(login(d5, TP_ROLE_DOWNLOAD, g_token), "download 5 login");
        d5.send_frame(2, TP_GET, get(0, 300, 1000));
        Batch b5;
        CHECK(read_batch(d5, 3000, &b5), "small batch");
        CHECK(b5.frames.size() >= 1 && b5.frames.size() < 6, "max_bytes limits the batch");
        d5.send_frame(2, TP_GET, get(b5.next, 15000, 1000));
        Batch b6;
        CHECK(read_batch(d5, 3000, &b6), "rest batch");
        CHECK(b5.next + b6.frames.size() == 6, "all 6 stored frames came in total, none twice");
    }
    /* 12. The upload DAQ reconnects: the new connection is accepted and its data goes on after the old one. */
    {
        Conn up2;
        uint64_t head = 0;
        CHECK(login(up2, TP_ROLE_UPLOAD, g_token, 0, &head), "upload reconnect");
        CHECK(head == 6, "the head continues");
        std::vector<uint8_t> st;
        st.push_back((uint8_t)-60); st.push_back(1); tp::put32(st, 7); tp::put32(st, 0); tp::put32(st, 99); tp::put16(st, 1);
        up2.send_frame(1, TP_STATUS, st);
        up2.send_frame(1, TP_DATA_PT, records(7000, 30));
        Batch b7;
        dn.send_frame(2, TP_GET, get(6, 8192, 2000));
        CHECK(read_batch(dn, 3000, &b7), "after reconnect");
        CHECK(b7.frames.size() >= 1, "data after reconnect");
    }
    /* 13. A download client may not send data. */
    {
        Conn d6;
        CHECK(login(d6, TP_ROLE_DOWNLOAD, g_token), "download 6 login");
        d6.send_frame(2, TP_DATA_PT, records(1, 3));
        uint8_t t = 0; std::vector<uint8_t> p;
        CHECK(d6.recv_frame(2000, &t, &p) && t == TP_NAK, "download client sends data: NAK");
    }

    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
