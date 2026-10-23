#include "chain.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"

#include "board.h"
#include "io.h"
#include "sd_log.h"

#define SYNC0        0xA5
#define SYNC1        0x5A
#define HDR          5U                   /* sync 2, device, type, count */
#define CRC_LEN      4U
#define MAX_RECS     32U
#define MAX_PKT      (HDR + MAX_RECS * 9U + CRC_LEN)
#define OWN_RECS     16U
#define MB_SIZE      4096U

#ifdef DAQ_CHAIN_SELFTEST
/* Test build: UART1 is looped back inside the chip, and the board sends as the device (ID xor 1). It then
   receives its own packets as the packets of another board. */
#define OWN_TX_ID    ((uint8_t)(io_addr() ^ 1U))
#else
#define OWN_TX_ID    io_addr()
#endif

static const char *TAG = "chain";
static const uart_port_t s_uart[2] = { UART_NUM_1, UART_NUM_2 };
static const int s_tx_pin[2] = { SERIAL1_TX_PIN, SERIAL2_TX_PIN };
static const int s_rx_pin[2] = { SERIAL1_RX_PIN, SERIAL2_RX_PIN };

static MessageBufferHandle_t s_mb[2];           /* packets waiting for TX, per port */
static StaticMessageBuffer_t s_mb_struct[2];
static uint8_t s_mb_store[2][MB_SIZE + 1];
static chain_stats_t s_st;

typedef struct { uint8_t buf[MAX_PKT + 4]; size_t n; } rx_t;
static rx_t s_rx[2];

static uint8_t s_batch[OWN_RECS * sizeof(record_t)];
static unsigned s_batch_n;

void chain_get_stats(chain_stats_t *out) { *out = s_st; }

static size_t rec_size(uint8_t type)
{
    return type == CHAIN_TYPE_ADS ? 8U : type == CHAIN_TYPE_LOADCELL ? 9U : 0U;
}

static void send_to(int port, const uint8_t *pkt, size_t len)
{
    if (xMessageBufferSend(s_mb[port], pkt, len, 0) != len) { s_st.tx_drop++; }
}

static void tx_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    static uint8_t buf[2][MAX_PKT + 4];
    for (;;) {
        size_t n = xMessageBufferReceive(s_mb[port], buf[port], sizeof buf[port], portMAX_DELAY);
        if (n) { uart_write_bytes(s_uart[port], buf[port], n); }
    }
}

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void send_own(void)
{
    uint8_t pkt[HDR + OWN_RECS * sizeof(record_t) + CRC_LEN];
    size_t pay = s_batch_n * sizeof(record_t), len = HDR + pay + CRC_LEN;
    pkt[0] = SYNC0; pkt[1] = SYNC1; pkt[2] = OWN_TX_ID; pkt[3] = CHAIN_TYPE_ADS; pkt[4] = (uint8_t)s_batch_n;
    memcpy(pkt + HDR, s_batch, pay);
    put_u32(pkt + HDR + pay, esp_rom_crc32_le(0, pkt + 2, (uint32_t)(HDR - 2U + pay)));
    send_to(0, pkt, len);
    send_to(1, pkt, len);
    s_st.tx_packets++;
    s_batch_n = 0;
}

void chain_push_ads(const record_t *r, bool recording)
{
    if (recording) {
        memcpy(s_batch + s_batch_n * sizeof *r, r, sizeof *r);
        if (++s_batch_n == OWN_RECS) { send_own(); }
    } else if (s_batch_n) {
        send_own();
    }
}

static void shift(rx_t *s, size_t k)
{
    s->n -= k;
    memmove(s->buf, s->buf + k, s->n);
}

static void handle(int port, const uint8_t *pkt, size_t len)
{
    uint8_t dev = pkt[2] & 7U, type = pkt[3], cnt = pkt[4];
    s_st.rx_ok[port]++;
    if (dev == io_addr()) { s_st.own_dropped++; return; }
    send_to(1 - port, pkt, len);                  /* repeat on the other port */
    s_st.forwarded++;
    if (type == CHAIN_TYPE_ADS) { sd_log_push_remote(dev, pkt + HDR, (size_t)cnt * rec_size(type)); }
}

/* One received byte. The state is the buffer: wrong bytes at the start are removed until a whole valid
   packet is at the start. */
static void rx_byte(int port, uint8_t b)
{
    rx_t *s = &s_rx[port];
    s->buf[s->n++] = b;
    while (s->n) {
        if (s->buf[0] != SYNC0) { shift(s, 1); continue; }
        if (s->n >= 2 && s->buf[1] != SYNC1) { shift(s, 1); continue; }
        if (s->n < HDR) { return; }
        size_t rs = rec_size(s->buf[3]), cnt = s->buf[4];
        if (rs == 0 || cnt == 0 || cnt > MAX_RECS) { s_st.rx_bad[port]++; shift(s, 1); continue; }
        size_t need = HDR + cnt * rs + CRC_LEN;
        if (s->n < need) { return; }
        if (get_u32(s->buf + need - CRC_LEN) == esp_rom_crc32_le(0, s->buf + 2, (uint32_t)(need - CRC_LEN - 2U))) {
            handle(port, s->buf, need);
            shift(s, need);
        } else {
            s_st.rx_bad[port]++;
            shift(s, 1);
        }
    }
}

static void rx_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    uint8_t b[128];
    for (;;) {
        int n = uart_read_bytes(s_uart[port], b, sizeof b, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) { rx_byte(port, b[i]); }
    }
}

void chain_start(void)
{
    const uart_config_t uc = { .baud_rate = CHAIN_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                               .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                               .source_clk = UART_SCLK_DEFAULT };
    for (int p = 0; p < 2; p++) {
        s_mb[p] = xMessageBufferCreateStatic(MB_SIZE, s_mb_store[p], &s_mb_struct[p]);
        uart_driver_install(s_uart[p], 2048, 0, 0, NULL, 0);
        uart_param_config(s_uart[p], &uc);
        uart_set_pin(s_uart[p], s_tx_pin[p], s_rx_pin[p], UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        xTaskCreatePinnedToCore(tx_task, "ch_tx", 3072, (void *)(intptr_t)p, 3, NULL, 0);
        xTaskCreatePinnedToCore(rx_task, "ch_rx", 3072, (void *)(intptr_t)p, 4, NULL, 0);
    }
#ifdef DAQ_CHAIN_SELFTEST
    uart_set_loop_back(UART_NUM_1, true);
#endif
    ESP_LOGI(TAG, "chain on UART1/UART2 at %d baud", CHAIN_BAUD);
}
