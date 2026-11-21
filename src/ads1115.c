#include "ads1115.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "board.h"
#include "record.h"

#define ADS_ADDR        0x48
#define REG_CONVERT     0x00
#define REG_CONFIG      0x01
#define REG_LOTHRESH    0x02
#define REG_HITHRESH    0x03

/* As the sketch: OS=1 (0x8000) | MUX=AIN0 against GND (0x4000) | PGA 6.144 V (0) | continuous (0) | 860 SPS (0x00E0)
   | comparator queue 00 = RDY pin. MUX bits 000 would read AIN0 minus AIN1 (a floating pin: always about 0). */
#define ADS_CONFIG_BASE 0x80E0     /* the same without the MUX bits (14..12) */
#define MUX_PT          4          /* AIN0 against GND */
#define MUX_NTC0        6          /* AIN2 against GND */
#define MUX_NTC1        7          /* AIN3 against GND */
#define ADS_CONFIG      (ADS_CONFIG_BASE | (MUX_PT << 12))
#define BACKUP_MS       3          /* no ready edge for 3 ms: read anyway (as the sketch) */

/* Two modes, saved in the flash (command NTC MODE), applied after a reset:
   - PT (default): the PT stream on AIN0 at +/-6.144 V. Nothing else: the thermistors are never read.
   - NTC only (a DAQ with thermistors and no PT): AIN2 and AIN3 at +/-4.096 V, 50 readings/s, no PT samples.
   The thermistor loop writes a new MUX for each channel, throws away the first NTC_DROP conversions after each write, and keeps
   the next NTC_KEEP (average). The first conversion after a write belongs to the old channel (the TI data sheet and forum answers:
   a new Config value waits for the conversion that runs), and the second one is thrown away for the input to settle. Throwing away
   both is also right if a write restarted the conversion, so the code does not depend on which one is true: the bench can lower
   NTC_DROP later. The ready edges are counted: a missed edge stops the cycle, and the next cycle starts again from a fresh edge. */
#define NTC_DROP        2
#define NTC_KEEP        4
#define NTC_PERIOD_US   20000      /* 50 readings/s */
#define NTC_IDLE_MS     1000       /* time between cycles while no thermistor is installed */
/* A thermistor is installed when its node is above a count that depends on its type (floating or missing: about 0 V).
   The node is 5 V x 560 / (R + 560), 125 uV per count (+/-4.096 V), and the limit is the voltage at 0 C (B 3950):
     10 kohm NTC:  R = 33.6 kohm   -> 82 mV  -> 656 counts
     100 kohm NTC: R = 336 kohm    -> 8.3 mV -> 66 counts (a floating pin reads 0 to a few counts) */
static const int32_t MIN_COUNTS[2] = { 656, 66 };
#define NTC_TYPE_FLAG   0x8000U    /* in each raw half of kind 13 (record.h): 1 = 100 kohm thermistor */

#define NTC_MAGIC       0x4E544301U
#define NVS_NS          "daq"
#define NVS_KEY         "ntc"
typedef struct { uint32_t magic; uint8_t only, type[2], pad; } ntc_store_t;

static const char *TAG = "ads";
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_task;
static ads_cb_t s_cb;
static ntc_cb_t s_ncb;
static portMUX_TYPE s_nmux = portMUX_INITIALIZER_UNLOCKED;
static ntc_stats_t s_nst;
static bool s_only;                            /* running mode: no PT (set once at the start) */
static ntc_cfg_t s_saved;                      /* the flash copy */
static volatile uint8_t s_type[2];             /* 0 = 10 kohm, 1 = 100 kohm; changes at once */
static int64_t s_last_us;
static volatile uint32_t s_edge_us;
static volatile bool s_running;

bool ads1115_running(void) { return s_running; }
bool ads1115_only_running(void) { return s_only; }

static void IRAM_ATTR rdy_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    (void)arg;
    s_edge_us = (uint32_t)esp_timer_get_time();
    vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) { portYIELD_FROM_ISR(); }
}

static esp_err_t write_reg(uint8_t reg, uint16_t v)
{
    uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return i2c_master_transmit(s_dev, b, sizeof b, 20);
}

static esp_err_t read_conv(int16_t *out)
{
    uint8_t reg = REG_CONVERT, r[2];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &reg, 1, r, 2, 20);
    if (e == ESP_OK) { *out = (int16_t)((r[0] << 8) | r[1]); }
    return e;
}

static int configure(i2c_master_bus_handle_t bus)
{
    if (write_reg(REG_HITHRESH, 0x8000) || write_reg(REG_LOTHRESH, 0x0000) || write_reg(REG_CONFIG, ADS_CONFIG)) {
        return -1;
    }
    (void)bus;
    return 0;
}

static i2c_master_bus_handle_t s_bus;

/* AIN2 or AIN3 against GND at +/-4.096 V (PGA bits 11..9 = 001). Only the thermistor loop uses this. */
static esp_err_t set_ntc_mux(int mux)
{
    return write_reg(REG_CONFIG, (uint16_t)(ADS_CONFIG_BASE | 0x0200U | ((unsigned)mux << 12)));
}

void ads1115_ntc_stats(ntc_stats_t *out)
{
    portENTER_CRITICAL(&s_nmux);
    *out = s_nst;
    out->age_ms = s_nst.have ? (uint32_t)((esp_timer_get_time() - s_last_us) / 1000) : 0U;
    portEXIT_CRITICAL(&s_nmux);
}

/* Exactly one new conversion: false when no edge came (timeout), more than one came (a missed sample: the order is lost),
   or the read failed. v can be NULL: the result is not needed. */
static bool wait_conv(int16_t *v, uint32_t *ts)
{
    uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BACKUP_MS));
    if (n != 1U) { return false; }
    *ts = s_edge_us;
    return v == NULL || read_conv(v) == ESP_OK;
}

/* One cycle: AIN2, then AIN3. The sum and the time of the first kept conversion go to sum[] and ts. False: the cycle failed. */
static bool ntc_cycle(int32_t sum[2], uint32_t *ts)
{
    static const int mux[2] = { MUX_NTC0, MUX_NTC1 };
    uint32_t t;
    (void)ulTaskNotifyTake(pdTRUE, 0);                           /* forget old edges, then start at a fresh one: the write is */
    if (!wait_conv(NULL, &t)) { return false; }                  /* 0.75 ms before the next edge, so no edge falls in the write */
    for (int ch = 0; ch < 2; ch++) {
        sum[ch] = 0;
        if (set_ntc_mux(mux[ch]) != ESP_OK) { return false; }
        if (ulTaskNotifyTake(pdTRUE, 0) != 0U) { return false; }   /* an edge came during the write: the order is not known */
        for (unsigned i = 0; i < NTC_DROP + NTC_KEEP; i++) {
            int16_t v = 0;
            bool keep = i >= NTC_DROP;
            if (!wait_conv(keep ? &v : NULL, &t)) { return false; }
            if (keep) {
                if (ch == 0 && i == NTC_DROP) { *ts = t; }
                sum[ch] += v;
            }
        }
    }
    return true;
}

static void ntc_loop(void)
{
    int64_t next = esp_timer_get_time() + 200000;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (next > now) { vTaskDelay(pdMS_TO_TICKS((next - now + 999) / 1000)); }
        now = esp_timer_get_time();
        int32_t sum[2];
        ntc_sample_t ns = { 0, { REC_NTC_ABSENT, REC_NTC_ABSENT } };
        bool ok = ntc_cycle(sum, &ns.timestamp_us);
        portENTER_CRITICAL(&s_nmux);
        if (ok) { s_nst.scans++; } else { s_nst.failed++; }
        portEXIT_CRITICAL(&s_nmux);
        if (!ok) { next = now + (int64_t)NTC_IDLE_MS * 1000; continue; }
        bool any = false;
        for (int c = 0; c < 2; c++) {
            unsigned t = s_type[c] ? 1U : 0U;
            int32_t avg = (sum[c] + (int32_t)(NTC_KEEP / 2U)) / (int32_t)NTC_KEEP;
            if (avg >= MIN_COUNTS[t] && avg < (int32_t)NTC_TYPE_FLAG) {
                ns.raw[c] = (uint16_t)(avg | (t ? NTC_TYPE_FLAG : 0U));
                any = true;
            }
        }
        if (!any) { next = now + (int64_t)NTC_IDLE_MS * 1000; continue; }
        next += NTC_PERIOD_US;                                     /* 50 Hz, no drift */
        if (next < now) { next = now; }
        portENTER_CRITICAL(&s_nmux);
        s_nst.have = true; s_nst.last = ns; s_last_us = now;
        portEXIT_CRITICAL(&s_nmux);
        if (s_ncb) { s_ncb(&ns); }
    }
}

static void pt_loop(void)
{
    for (;;) {
        ads_sample_t s;
        uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BACKUP_MS));
        s.timestamp_us = n ? s_edge_us : (uint32_t)esp_timer_get_time();
        if (read_conv(&s.reading) == ESP_OK) { s_cb(&s); }
    }
}

static void ads_task(void *arg)
{
    (void)arg;
    /* The analog 5 V can come after the start: try again each second until the chip answers. */
    for (bool warned = false; configure(s_bus) != 0; warned = true) {
        if (!warned) { ESP_LOGW(TAG, "no answer from the ADS1115 at 0x48, trying again each second"); }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_only) { ESP_LOGW(TAG, "ADS1115 running in THERMISTOR-ONLY mode: NO PT samples are made (NTC MODE PT to change)"); }
    else { ESP_LOGI(TAG, "ADS1115 running"); }
    s_running = true;

    const gpio_config_t gc = { .pin_bit_mask = 1ULL << I2C0_RDY_PIN, .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&gc);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(I2C0_RDY_PIN, rdy_isr, NULL);

    if (s_only) { ntc_loop(); } else { pt_loop(); }
}

/* ---- the settings (flash) ---- */

static bool store_write(void)
{
    nvs_handle_t h;
    ntc_store_t st = { NTC_MAGIC, s_saved.only, { s_saved.type[0], s_saved.type[1] }, 0 };
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, NVS_KEY, &st, sizeof st) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

static void store_load(void)
{
    nvs_handle_t h;
    ntc_store_t st;
    size_t len = sizeof st;
    memset(&s_saved, 0, sizeof s_saved);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &st, &len) == ESP_OK && len == sizeof st && st.magic == NTC_MAGIC) {
            s_saved.only = st.only != 0U;
            s_saved.type[0] = st.type[0] ? 1U : 0U;
            s_saved.type[1] = st.type[1] ? 1U : 0U;
        }
        nvs_close(h);
    }
}

void ads1115_ntc_cfg(ntc_cfg_t *saved, bool *running_only)
{
    *saved = s_saved;
    *running_only = s_only;
}

bool ads1115_ntc_set_only(bool only)
{
    s_saved.only = only;
    return store_write();
}

bool ads1115_ntc_set_type(int ch, int type)
{
    if (ch < 0 || ch > 1) { return false; }
    s_saved.type[ch] = type ? 1U : 0U;
    s_type[ch] = s_saved.type[ch];
    return store_write();
}

int ads1115_start(ads_cb_t cb, ntc_cb_t ncb)
{
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = I2C0_SDA_PIN, .scl_io_num = I2C0_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    const i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS_ADDR,
                                     .scl_speed_hz = 400000 };
    s_cb = cb;
    s_ncb = ncb;
    store_load();
    s_only = s_saved.only;
    s_type[0] = s_saved.type[0];
    s_type[1] = s_saved.type[1];
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK || i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C set-up failed");
        return -1;
    }
    xTaskCreatePinnedToCore(ads_task, "ads", 4096, NULL, 6, &s_task, 1);
    return 0;
}
