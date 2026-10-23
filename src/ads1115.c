#include "ads1115.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

#define ADS_ADDR        0x48
#define REG_CONVERT     0x00
#define REG_CONFIG      0x01
#define REG_LOTHRESH    0x02
#define REG_HITHRESH    0x03

/* OS=1, MUX=AIN0, PGA=6.144 V, continuous, 860 SPS, comparator queue = RDY pin */
#define ADS_CONFIG      0x80E0
#define BACKUP_MS       3          /* no ready edge for 3 ms: read anyway (as the sketch) */

static const char *TAG = "ads";
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_task;
static ads_cb_t s_cb;
static volatile uint32_t s_edge_us;

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

static void ads_task(void *arg)
{
    (void)arg;
    /* The analog 5 V can come after the start: try again each second until the chip answers. */
    for (bool warned = false; configure(s_bus) != 0; warned = true) {
        if (!warned) { ESP_LOGW(TAG, "no answer from the ADS1115 at 0x48, trying again each second"); }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "ADS1115 running");

    const gpio_config_t gc = { .pin_bit_mask = 1ULL << I2C0_RDY_PIN, .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&gc);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(I2C0_RDY_PIN, rdy_isr, NULL);

    for (;;) {
        ads_sample_t s;
        uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BACKUP_MS));
        s.timestamp_us = n ? s_edge_us : (uint32_t)esp_timer_get_time();
        if (read_conv(&s.reading) == ESP_OK) { s_cb(&s); }
    }
}

int ads1115_start(ads_cb_t cb)
{
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = I2C0_SDA_PIN, .scl_io_num = I2C0_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    const i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS_ADDR,
                                     .scl_speed_hz = 400000 };
    s_cb = cb;
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK || i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C set-up failed");
        return -1;
    }
    xTaskCreatePinnedToCore(ads_task, "ads", 4096, NULL, 6, &s_task, 1);
    return 0;
}
