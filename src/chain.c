#include "chain.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ads1115.h"
#include "board.h"
#include "io.h"
#include "nau7802.h"
#include "sd_log.h"
#include "telem.h"

#define SYNC0        0xA5
#define SYNC1        0x5A
#define HDR          5U                   /* sync 2, device, type, count */
#define CRC_LEN      4U
#define MAX_RECS     32U
#define MAX_PKT      (HDR + MAX_RECS * sizeof(record_t) + CRC_LEN)
#define OWN_RECS     16U
#define MB_SIZE      4096U
#define HELLO_MS     1000U                /* heartbeat interval */

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
static SemaphoreHandle_t s_tx_lock;             /* a message buffer allows one writer only; three tasks send */
static StaticSemaphore_t s_tx_lock_struct;
static chain_stats_t s_st;
static volatile TickType_t s_seen[8];           /* last valid packet of each device ID; 0 = never */
static volatile uint8_t s_flags[8];             /* last heartbeat flags of each device ID */

typedef struct { uint8_t buf[MAX_PKT + 4]; size_t n; } rx_t;
static rx_t s_rx[2];

/* The own records wait here, one batch for each sensor. Only the sink in main.c calls these, under its lock. */
typedef struct { uint8_t type; unsigned n; uint8_t buf[OWN_RECS * sizeof(record_t)]; } batch_t;
static batch_t s_ads = { .type = CHAIN_TYPE_ADS }, s_lc = { .type = CHAIN_TYPE_LOADCELL };

void chain_get_stats(chain_stats_t *out) { *out = s_st; }

uint8_t chain_seen(uint32_t max_age_ms, uint8_t flags[8])
{
    TickType_t now = xTaskGetTickCount();
    uint8_t mask = 0;
    for (int i = 0; i < 8; i++) {
        TickType_t t = s_seen[i];
        if (t && now - t <= pdMS_TO_TICKS(max_age_ms)) { mask |= (uint8_t)(1U << i); }
        if (flags) { flags[i] = s_flags[i]; }
    }
    return mask;
}

static size_t rec_size(uint8_t type)
{
    return (type == CHAIN_TYPE_ADS || type == CHAIN_TYPE_LOADCELL || type == CHAIN_TYPE_CAL) ? sizeof(record_t)
         : type == CHAIN_TYPE_HELLO ? 1U : 0U;
}

static void send_to(int port, const uint8_t *pkt, size_t len)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);       /* held for one copy only */
    if (xMessageBufferSend(s_mb[port], pkt, len, 0) != len) { s_st.tx_drop++; }
    xSemaphoreGive(s_tx_lock);
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

/* Make one packet with the own ID and send it out of both ports. */
static void send_pkt(uint8_t type, uint8_t count, const uint8_t *data, size_t pay)
{
    uint8_t pkt[HDR + OWN_RECS * sizeof(record_t) + CRC_LEN];
    size_t len = HDR + pay + CRC_LEN;
    pkt[0] = SYNC0; pkt[1] = SYNC1; pkt[2] = OWN_TX_ID; pkt[3] = type; pkt[4] = count;
    memcpy(pkt + HDR, data, pay);
    put_u32(pkt + HDR + pay, esp_rom_crc32_le(0, pkt + 2, (uint32_t)(HDR - 2U + pay)));
    send_to(0, pkt, len);
    send_to(1, pkt, len);
    s_st.tx_packets++;
}

/* The heartbeat: the other boards see this board also when its sensor sends no samples. */
static void hello_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t f = (uint8_t)((rec_get() ? CHAIN_HELLO_REC : 0U) | (io_auto_mode() ? CHAIN_HELLO_AUTO : 0U) |
                              (ads1115_running() ? CHAIN_HELLO_SENSOR : 0U) | (sd_log_mounted() ? CHAIN_HELLO_SD : 0U) |
                              (nau7802_running() ? CHAIN_HELLO_LOADCELL : 0U) |
                              (telem_link_up() ? CHAIN_HELLO_WIFI : 0U));
        send_pkt(CHAIN_TYPE_HELLO, 1, &f, 1);
        vTaskDelay(pdMS_TO_TICKS(HELLO_MS));
    }
}

static void batch_push(batch_t *b, const record_t *r)
{
    memcpy(b->buf + b->n * sizeof *r, r, sizeof *r);
    if (++b->n == OWN_RECS) {
        send_pkt(b->type, OWN_RECS, b->buf, sizeof b->buf);
        b->n = 0;
    }
}

void chain_push_ads(const record_t *r) { batch_push(&s_ads, r); }
void chain_push_lc(const record_t *r)  { batch_push(&s_lc, r); }

void chain_push_cal(const record_t *r, unsigned n)
{
    if (n > 0U && n <= OWN_RECS) {
        send_pkt(CHAIN_TYPE_CAL, (uint8_t)n, (const uint8_t *)r, n * sizeof *r);
        s_st.tx_cal++;
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
    s_seen[dev] = xTaskGetTickCount() | 1U;       /* | 1: never 0 */
    if (type == CHAIN_TYPE_CAL) { s_st.rx_cal++; }
    if (type == CHAIN_TYPE_HELLO) { s_flags[dev] = pkt[HDR]; }
    send_to(1 - port, pkt, len);                  /* repeat on the other port */
    s_st.forwarded++;
    if ((type == CHAIN_TYPE_ADS || type == CHAIN_TYPE_LOADCELL || type == CHAIN_TYPE_CAL) && rec_get()) {   /* recording = saving */
        sd_log_push_remote(dev, pkt + HDR, (size_t)cnt * rec_size(type));
    }
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
    s_tx_lock = xSemaphoreCreateMutexStatic(&s_tx_lock_struct);
    for (int p = 0; p < 2; p++) {
        s_mb[p] = xMessageBufferCreateStatic(MB_SIZE, s_mb_store[p], &s_mb_struct[p]);
        uart_driver_install(s_uart[p], 2048, 0, 0, NULL, 0);
        uart_param_config(s_uart[p], &uc);
        uart_set_pin(s_uart[p], s_tx_pin[p], s_rx_pin[p], UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        xTaskCreatePinnedToCore(tx_task, "ch_tx", 3072, (void *)(intptr_t)p, 3, NULL, 0);
        xTaskCreatePinnedToCore(rx_task, "ch_rx", 3072, (void *)(intptr_t)p, 4, NULL, 0);
    }
    xTaskCreatePinnedToCore(hello_task, "ch_hello", 2560, NULL, 2, NULL, 0);
#ifdef DAQ_CHAIN_SELFTEST
    uart_set_loop_back(UART_NUM_1, true);
#endif
    ESP_LOGI(TAG, "chain on UART1/UART2 at %d baud", CHAIN_BAUD);
}
