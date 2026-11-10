/* Telemetry server: receives frames from the upload DAQ and gives them to the download DAQ (see telem_proto.h).
   One TCP port, one thread, poll(). No libraries. Settings come from the environment:
     TELEM_PORT     TCP port (default 48613)
     TELEM_TOKEN    shared token, 16 characters at most (default "changeme", and the server warns)
     TELEM_DATA_DIR folder for the archive file (default /data). Empty = no archive.
     TELEM_RING_MB  size of the frame ring in memory (default 64) */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#include "tp_util.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string env_or(const char *name, const char *def)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string(def);
}

struct Entry { uint64_t idx; std::vector<uint8_t> frame; int64_t rx_ms; };

struct Client {
    int fd = -1;
    std::string peer;
    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;
    size_t tx_off = 0;
    uint8_t role = 0;
    bool authed = false;
    bool dead = false;
    bool pending = false;       /* a GET waits for data */
    bool want_cal = false;      /* the first reply must carry the last CAL frame */
    uint64_t cursor = 0;
    size_t max_bytes = 0;
    int64_t deadline = 0;
    int64_t last_rx = 0;
};

struct Stats {
    uint64_t frames = 0, rec_pt = 0, rec_lc = 0, bytes_in = 0, bytes_out = 0, bad_crc = 0, uploads = 0, downloads = 0;
    int64_t last_upload_rx = 0;
    bool have_status = false;
    tp_status_t status{};
};

std::deque<Entry> g_ring;
size_t g_ring_bytes = 0, g_ring_cap = 64U << 20;
uint64_t g_head = 0;                    /* the index of the next frame */
std::vector<uint8_t> g_last_cal;
std::vector<Client> g_clients;
Stats g_st;
FILE *g_archive = nullptr;
uint8_t g_token[TP_TOKEN_LEN];

void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *fmt, ...)
{
    char tbuf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(tbuf, sizeof tbuf, "%H:%M:%S", std::gmtime(&t));
    std::printf("[%s] ", tbuf);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

uint64_t tail_idx() { return g_ring.empty() ? g_head : g_ring.front().idx; }

void queue_tx(Client &c, const std::vector<uint8_t> &f)
{
    if (c.tx.size() - c.tx_off > (1U << 20)) { c.dead = true; return; }      /* a client that does not read: drop it */
    c.tx.insert(c.tx.end(), f.begin(), f.end());
    g_st.bytes_out += f.size();
}

void send_nak(Client &c, uint8_t why)
{
    queue_tx(c, tp::build(0, TP_NAK, &why, 1));
    /* try to push it out now; the connection closes after it */
    ssize_t n = ::send(c.fd, c.tx.data() + c.tx_off, c.tx.size() - c.tx_off, MSG_NOSIGNAL | MSG_DONTWAIT);
    (void)n;
    c.dead = true;
}

void store(const uint8_t *frame, size_t n, uint8_t type, const uint8_t *pay, size_t len)
{
    Entry e{ g_head++, std::vector<uint8_t>(frame, frame + n), now_ms() };
    g_ring_bytes += n;
    g_ring.push_back(std::move(e));
    while (g_ring_bytes > g_ring_cap && g_ring.size() > 1) { g_ring_bytes -= g_ring.front().frame.size(); g_ring.pop_front(); }
    if (type == TP_CAL) { g_last_cal.assign(frame, frame + n); }
    if (type == TP_STATUS && len >= sizeof(tp_status_t)) { std::memcpy(&g_st.status, pay, sizeof g_st.status); g_st.have_status = true; }
    if (g_archive) { std::fwrite(frame, 1, n, g_archive); }
}

/* Answer a GET: whole stored frames from the cursor on. */
void reply_batch(Client &c)
{
    std::vector<uint8_t> pay;
    uint64_t start = std::max(std::min(c.cursor, g_head), tail_idx());
    uint64_t next = start;
    int64_t age = g_ring.empty() ? -1 : now_ms() - g_ring.back().rx_ms;
    tp::put64(pay, 0);                                  /* next cursor, filled below */
    tp::put32(pay, age < 0 ? TP_AGE_NONE : (uint32_t)std::min<int64_t>(age, 0xFFFFFFF0LL));
    if (c.want_cal) {
        if (!g_last_cal.empty() && g_last_cal.size() <= c.max_bytes) { pay.insert(pay.end(), g_last_cal.begin(), g_last_cal.end()); }
        c.want_cal = false;
    }
    if (start < g_head && !g_ring.empty()) {
        const size_t limit = std::min<size_t>(c.max_bytes + 12U, TP_MAX_PAYLOAD);     /* the first frame always goes in */
        for (size_t i = (size_t)(start - g_ring.front().idx); i < g_ring.size(); i++) {
            const Entry &e = g_ring[i];
            if (pay.size() + e.frame.size() > limit && next > start) { break; }
            pay.insert(pay.end(), e.frame.begin(), e.frame.end());
            next = e.idx + 1;
        }
    }
    for (int i = 0; i < 8; i++) { pay[(size_t)i] = (uint8_t)(next >> (8 * i)); }
    c.cursor = next;
    c.pending = false;
    queue_tx(c, tp::build(0, TP_BATCH, pay.data(), pay.size()));
}

void handle_frame(Client &c, const tp::View &v, const uint8_t *frame, size_t n)
{
    g_st.frames++;
    if (!c.authed) {
        if (v.type != TP_HELLO || v.len != sizeof(tp_hello_t)) { send_nak(c, TP_NAK_FRAME); return; }
        tp_hello_t h;
        std::memcpy(&h, v.pay, sizeof h);
        if (std::memcmp(h.token, g_token, TP_TOKEN_LEN) != 0) { log_line("%s: bad token", c.peer.c_str()); send_nak(c, TP_NAK_TOKEN); return; }
        if (h.role != TP_ROLE_UPLOAD && h.role != TP_ROLE_DOWNLOAD) { send_nak(c, TP_NAK_ROLE); return; }
        c.authed = true;
        c.role = h.role;
        std::vector<uint8_t> a;
        tp::put64(a, g_head);
        queue_tx(c, tp::build(0, TP_ACK, a.data(), a.size()));
        if (h.role == TP_ROLE_UPLOAD) { g_st.uploads++; } else { g_st.downloads++; }
        log_line("%s: %s connected (head=%llu)", c.peer.c_str(), h.role == TP_ROLE_UPLOAD ? "UPLOAD" : "DOWNLOAD", (unsigned long long)g_head);
        return;
    }
    if (c.role == TP_ROLE_UPLOAD) {
        bool ok = false;
        switch (v.type) {
        case TP_DATA_PT: ok = v.len > 0 && v.len % TP_REC_SIZE == 0; if (ok) { g_st.rec_pt += v.len / TP_REC_SIZE; } break;
        case TP_DATA_LC: ok = v.len > 0 && v.len % TP_REC_SIZE == 0; if (ok) { g_st.rec_lc += v.len / TP_REC_SIZE; } break;
        case TP_CAL:     ok = v.len == TP_CAL_RECS * TP_REC_SIZE; break;
        case TP_STATUS:  ok = v.len == sizeof(tp_status_t); break;
        default: break;
        }
        if (!ok || v.len > TP_MAX_UPLOAD) { send_nak(c, TP_NAK_FRAME); return; }
        store(frame, n, v.type, v.pay, v.len);
        g_st.last_upload_rx = now_ms();
    } else {                                            /* DOWNLOAD */
        if (v.type != TP_GET || v.len != sizeof(tp_get_t)) { send_nak(c, TP_NAK_FRAME); return; }
        tp_get_t g;
        std::memcpy(&g, v.pay, sizeof g);
        c.max_bytes = g.max_bytes == 0 ? 8192U : std::min<size_t>(g.max_bytes, 15000U);
        if (g.cursor == TP_CURSOR_NOW) { c.cursor = g_head; c.want_cal = true; }
        else { c.cursor = g.cursor; }
        uint64_t start = std::max(std::min(c.cursor, g_head), tail_idx());
        if (start < g_head || c.want_cal) { reply_batch(c); }
        else {
            c.pending = true;
            c.cursor = start;
            c.deadline = now_ms() + std::min<int64_t>(g.wait_ms, 10000);
        }
    }
}

void parse_client(Client &c)
{
    size_t pos = 0;
    while (pos < c.rx.size() && !c.dead) {
        size_t used = 0;
        tp::View v;
        tp::Parse r = tp::parse(c.rx.data() + pos, c.rx.size() - pos, &used, &v);
        if (r == tp::Parse::Need) { break; }
        if (r == tp::Parse::Bad) {
            if (c.rx[pos] == TP_SYNC0 && c.rx.size() - pos >= TP_HDR) { g_st.bad_crc++; }   /* a frame that fails; a stray byte is not counted */
            pos += used;
            continue;
        }
        handle_frame(c, v, c.rx.data() + pos, used);
        pos += used;
    }
    c.rx.erase(c.rx.begin(), c.rx.begin() + (long)pos);
    if (c.rx.size() > 2U * (TP_MAX_PAYLOAD + 16U)) { c.dead = true; }
}

void print_stats()
{
    int up = 0, down = 0;
    for (const Client &c : g_clients) { if (c.authed && c.role == TP_ROLE_UPLOAD) { up++; } else if (c.authed) { down++; } }
    int64_t age = g_st.last_upload_rx ? now_ms() - g_st.last_upload_rx : -1;
    log_line("up=%d down=%d frames=%llu pt=%llu lc=%llu in=%lluB out=%lluB badcrc=%llu last_rx_age=%lldms ring=%zu rssi=%d drops=%u",
             up, down, (unsigned long long)g_st.frames, (unsigned long long)g_st.rec_pt, (unsigned long long)g_st.rec_lc,
             (unsigned long long)g_st.bytes_in, (unsigned long long)g_st.bytes_out, (unsigned long long)g_st.bad_crc,
             (long long)age, g_ring.size(), g_st.have_status ? g_st.status.rssi : 0, g_st.have_status ? (unsigned)g_st.status.drops : 0U);
}

int make_listener(int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) { ::close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

void accept_all(int lfd)
{
    for (;;) {
        sockaddr_in a{};
        socklen_t al = sizeof a;
        int fd = ::accept(lfd, reinterpret_cast<sockaddr *>(&a), &al);
        if (fd < 0) { return; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int one = 1, idle = 20, intvl = 5, cnt = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);       /* a client that vanished is found in about 35 s */
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
        Client c;
        c.fd = fd;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
        c.peer = std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
        c.last_rx = now_ms();
        g_clients.push_back(std::move(c));
    }
}

}  // namespace

int main()
{
    int port = std::atoi(env_or("TELEM_PORT", "48613").c_str());
    std::string token = env_or("TELEM_TOKEN", "changeme");
    std::string dir = env_or("TELEM_DATA_DIR", "/data");
    g_ring_cap = (size_t)std::max(1, std::atoi(env_or("TELEM_RING_MB", "64").c_str())) << 20;
    std::memset(g_token, 0, sizeof g_token);
    std::memcpy(g_token, token.data(), std::min<size_t>(token.size(), TP_TOKEN_LEN));
    if (token == "changeme") { log_line("WARNING: TELEM_TOKEN is not set. Anyone can send data."); }

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN);

    if (!dir.empty()) {
        char name[64];
        std::time_t t = std::time(nullptr);
        std::strftime(name, sizeof name, "/archive_%Y%m%d_%H%M%S.tlm", std::gmtime(&t));
        g_archive = std::fopen((dir + name).c_str(), "wb");
        if (!g_archive) { log_line("WARNING: cannot open the archive file in %s: no archive", dir.c_str()); }
        else { log_line("archive: %s%s", dir.c_str(), name); }
    }

    int lfd = make_listener(port);
    if (lfd < 0) { log_line("ERROR: cannot listen on port %d", port); return 1; }
    log_line("listening on port %d, ring %zu MB", port, g_ring_cap >> 20);

    int64_t last_stats = now_ms(), last_flush = now_ms();
    while (!g_stop) {
        std::vector<pollfd> p;
        p.push_back({ lfd, POLLIN, 0 });
        for (const Client &c : g_clients) { p.push_back({ c.fd, (short)(POLLIN | (c.tx_off < c.tx.size() ? POLLOUT : 0)), 0 }); }
        int64_t now = now_ms();
        int64_t wait = 1000;
        for (const Client &c : g_clients) { if (c.pending) { wait = std::min<int64_t>(wait, std::max<int64_t>(0, c.deadline - now)); } }
        ::poll(p.data(), p.size(), (int)wait);

        if (p[0].revents & POLLIN) { accept_all(lfd); }
        for (size_t i = 0; i + 1 < p.size() && i < g_clients.size(); i++) {
            Client &c = g_clients[i];
            short re = p[i + 1].revents;
            if (re & POLLIN) {
                uint8_t buf[16384];
                ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) { c.rx.insert(c.rx.end(), buf, buf + n); g_st.bytes_in += (uint64_t)n; c.last_rx = now_ms(); parse_client(c); }
                else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { c.dead = true; }
            } else if (re & (POLLERR | POLLHUP | POLLNVAL)) { c.dead = true; }
        }
        /* new data or a timeout: answer the waiting GETs */
        now = now_ms();
        for (Client &c : g_clients) {
            if (c.dead || !c.pending) { continue; }
            if (std::max(std::min(c.cursor, g_head), tail_idx()) < g_head || now >= c.deadline) { reply_batch(c); }
        }
        for (Client &c : g_clients) {
            if (c.dead) { continue; }
            while (c.tx_off < c.tx.size()) {
                ssize_t n = ::send(c.fd, c.tx.data() + c.tx_off, c.tx.size() - c.tx_off, MSG_NOSIGNAL);
                if (n > 0) { c.tx_off += (size_t)n; }
                else { if (n < 0 && errno != EAGAIN && errno != EINTR) { c.dead = true; } break; }
            }
            if (c.tx_off == c.tx.size()) { c.tx.clear(); c.tx_off = 0; }
            if (c.authed && c.role == TP_ROLE_UPLOAD && now - c.last_rx > 30000) { log_line("%s: upload idle for 30 s, closed", c.peer.c_str()); c.dead = true; }
            if (!c.authed && now - c.last_rx > 10000) { c.dead = true; }          /* no HELLO */
        }
        for (size_t i = 0; i < g_clients.size();) {
            if (g_clients[i].dead) {
                if (g_clients[i].authed) { log_line("%s: closed", g_clients[i].peer.c_str()); }
                ::close(g_clients[i].fd);
                g_clients.erase(g_clients.begin() + (long)i);
            } else { i++; }
        }
        if (now - last_flush >= 1000) { if (g_archive) { std::fflush(g_archive); } last_flush = now; }
        if (now - last_stats >= 10000) { print_stats(); last_stats = now; }
    }
    log_line("stop");
    if (g_archive) { std::fclose(g_archive); }
    return 0;
}
