#include "nau7802.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

#ifndef NAU_ADDR
#define NAU_ADDR        0x2A       /* env daq_nolc sets another address: the chip then does not answer */
#endif
#define REG_PU_CTRL     0x00
#define REG_CTRL1       0x01
#define REG_CTRL2       0x02
#define REG_OCAL1_B2    0x03
#define REG_ADCO_B2     0x12
#define REG_ADC         0x15
#define REG_PGA         0x1B
#define REG_POWER       0x1C
#define REG_REVISION    0x1F

#define PU_RR           0x01       /* register reset */
#define PU_PUD          0x02       /* digital power */
#define PU_PUA          0x04       /* analog power */
#define PU_PUR          0x08       /* power-up ready */
#define PU_CS           0x10       /* start the conversions */
#define PU_CR           0x20       /* a new result is ready */
#define PU_AVDDS        0x80       /* AVDD comes from the internal LDO */
#define CTRL2_CALS      0x04       /* start a calibration; the chip clears it at the end */
#define CTRL2_CAL_ERR   0x08

/* The settings. VLDO 4.5 V needs DVDD at 4.8 V or more (data sheet: DVDD = VLDO + 0.3 V).
   Other values: VLDO 000 4.5 V .. 111 2.4 V; gain 111 x128 .. 000 x1; CRS 111 320 SPS, 011 80, 010 40, 001 20, 000 10. */
#define NAU_CTRL1       0x07       /* CRP 0 = DRDY is high when ready | VLDO 000 = 4.5 V | gain 111 = x128 */
#define NAU_CTRL2       0x70       /* channel 1 | CRS 111 = 320 SPS | CALMOD 00 = internal offset calibration */
#define NAU_ADC         0x30       /* chopper clock off (data sheet 9.1) */
#define NAU_PGA         0x00       /* LDOMODE 0: the AVDD capacitors are ceramic, ESR below 1 ohm */
#define NAU_POWER       0x80       /* PGA_CAP_EN: uses the capacitor across VIN2P and VIN2N. Channel 2 is not used. */

#define LDO_MS          250        /* AVDD settles */
#define SETTLE_MS       100        /* the ADC settles after CS. 25 ms was too short: the calibration saw a full-scale value */
#define CAL_TRIES       500        /* 2 ms each */
#define OCAL_SIZE_MAX   0x10000U   /* a healthy chip needs some hundred counts. 0x7FFFFF: the calibration saw full scale */
#define DISCARD         6          /* conversions after a reset that are not valid (data sheet 8.8) */
#define BACKUP_MS       6          /* no ready edge for 2 periods: test the CR bit. Must be more than one period of NAU_CTRL2. */
#define STOP_MS         100        /* no result for this time: the chip lost power */

static const char *TAG = "nau";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_task;
static nau_cb_t s_cb;
static volatile uint32_t s_edge_us;
static volatile bool s_running, s_fault;
static nau_stats_t s_st;

bool nau7802_running(void) { return s_running; }
bool nau7802_fault(void)   { return s_fault; }
void nau7802_get_stats(nau_stats_t *out) { *out = s_st; }

static void IRAM_ATTR rdy_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    (void)arg;
    s_edge_us = (uint32_t)esp_timer_get_time();
    vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) { portYIELD_FROM_ISR(); }
}

static esp_err_t write_reg(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, sizeof b, 20);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *v, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, n, 20);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *v) { return read_regs(reg, v, 1); }

/* A read of REG_ADCO_B2 latches all 3 bytes. With no new result, it gives the old result again. */
static esp_err_t read_conv(int32_t *out)
{
    uint8_t r[3];
    esp_err_t e = read_regs(REG_ADCO_B2, r, sizeof r);
    if (e == ESP_OK) { *out = (int32_t)(((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8)) >> 8; }
    return e;
}

/* Data sheet 9.1. The internal calibration disconnects the inputs, so a load on the cell has no effect on it. */
static bool configure(void)
{
    uint8_t v = 0;
    int tries = 0;

    if (write_reg(REG_PU_CTRL, PU_RR) != ESP_OK) { return false; }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (write_reg(REG_PU_CTRL, PU_PUD) != ESP_OK) { return false; }
    do {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (read_reg(REG_PU_CTRL, &v) != ESP_OK) { return false; }
    } while (!(v & PU_PUR) && ++tries < 10);
    if (!(v & PU_PUR) || read_reg(REG_REVISION, &v) != ESP_OK || (v & 0x0F) != 0x0F) { return false; }

    if (write_reg(REG_CTRL1, NAU_CTRL1) || write_reg(REG_CTRL2, NAU_CTRL2) || write_reg(REG_ADC, NAU_ADC) ||
        write_reg(REG_PGA, NAU_PGA) || write_reg(REG_POWER, NAU_POWER) ||
        write_reg(REG_PU_CTRL, PU_AVDDS | PU_PUA | PU_PUD)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(LDO_MS));
    if (write_reg(REG_PU_CTRL, PU_AVDDS | PU_CS | PU_PUA | PU_PUD)) { return false; }
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    if (write_reg(REG_CTRL2, NAU_CTRL2 | CTRL2_CALS)) { return false; }
    tries = 0;
    do {
        vTaskDelay(pdMS_TO_TICKS(2));
        if (read_reg(REG_CTRL2, &v) != ESP_OK) { return false; }
    } while ((v & CTRL2_CALS) && ++tries < CAL_TRIES);
    if (v & (CTRL2_CALS | CTRL2_CAL_ERR)) { return false; }

    uint8_t o[3];
    if (read_regs(REG_OCAL1_B2, o, sizeof o) != ESP_OK) { return false; }
    s_st.ocal = ((uint32_t)o[0] << 16) | ((uint32_t)o[1] << 8) | o[2];
    /* A bad result moves all readings by up to full scale, with no other sign. Size 0: the calibration did nothing. */
    uint32_t size = s_st.ocal & 0x7FFFFFU;
    return size != 0U && size < OCAL_SIZE_MAX;
}

/* Gives samples until the chip stops. A read after a timeout tests CR first: with no new result, a read gives a copy. */
static void run(void)
{
    nau_sample_t s;
    unsigned skip = DISCARD, quiet_ms = 0;

    (void)read_conv(&s.reading);                  /* DRDY goes low, so the next result makes an edge */
    gpio_intr_enable(I2C1_RDY_PIN);
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BACKUP_MS))) {
            s.timestamp_us = s_edge_us;
        } else {
            uint8_t pu = 0;
            esp_err_t e = read_reg(REG_PU_CTRL, &pu);
            if (e != ESP_OK) { s_st.i2c_errors++; }
            if (e != ESP_OK || !(pu & PU_CR)) {
                quiet_ms += BACKUP_MS;
                if (quiet_ms >= STOP_MS) { break; }
                continue;
            }
            s.timestamp_us = (uint32_t)esp_timer_get_time();
            s_st.backup_reads++;
        }
        if (read_conv(&s.reading) != ESP_OK) { s_st.i2c_errors++; continue; }
        quiet_ms = 0;
        if (skip) { skip--; continue; }
        if (!s_running) { s_running = true; s_fault = false; ESP_LOGI(TAG, "NAU7802 running"); }
        s_st.samples++;
        s_cb(&s);
    }
    gpio_intr_disable(I2C1_RDY_PIN);
}

static void nau_task(void *arg)
{
    const gpio_config_t gc = { .pin_bit_mask = 1ULL << I2C1_RDY_PIN, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE };
    (void)arg;
    gpio_config(&gc);
    gpio_intr_disable(I2C1_RDY_PIN);              /* a pin with no sensor floats: no interrupts until the chip answers */
    gpio_isr_handler_add(I2C1_RDY_PIN, rdy_isr, NULL);   /* io_init() installed the service */

    for (bool warned = false;;) {
        /* The analog 5 V can come after the start, and a board can have no load cell: try again each second. */
        while (!configure()) {
            if (!warned) { ESP_LOGW(TAG, "NAU7802 at 0x2A: no answer or a bad calibration, trying again each second"); warned = true; }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        run();
        s_fault = s_running;
        s_running = false;
        s_st.restarts++;
        ESP_LOGW(TAG, "NAU7802 stopped, starting it again");
    }
}

int nau7802_start(nau_cb_t cb)
{
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_1, .sda_io_num = I2C1_SDA_PIN, .scl_io_num = I2C1_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    const i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = NAU_ADDR,
                                     .scl_speed_hz = 400000 };
    s_cb = cb;
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK || i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C set-up failed");
        return -1;
    }
    xTaskCreatePinnedToCore(nau_task, "nau", 4096, NULL, 6, &s_task, 0);
    return 0;
}
