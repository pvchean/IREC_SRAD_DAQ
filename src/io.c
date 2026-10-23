#include "io.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

#define SETTLE_MS   50           /* time for the pull-up to set the level of a floating pin */
#define DEBOUNCE_US 200000

static const char *TAG = "io";
static bool s_auto, s_files;
static uint8_t s_addr;
static volatile bool s_rec;
static volatile int64_t s_last_us;

static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    (void)arg;
    if (now - s_last_us > DEBOUNCE_US) {
        s_last_us = now;
        s_rec = !s_rec;
    }
}

static void pullup_input(int pin)
{
    const gpio_config_t gc = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&gc);
}

void io_init(void)
{
    static const int pins[] = { MODE1_PIN, MODE2_PIN, ADDR0_PIN, ADDR1_PIN, ADDR2_PIN };
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) { pullup_input(pins[i]); }
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    s_auto  = gpio_get_level(MODE1_PIN) != 0;
    s_files = gpio_get_level(MODE2_PIN) == 0;
    s_addr  = (uint8_t)((gpio_get_level(ADDR2_PIN) << 2) | (gpio_get_level(ADDR1_PIN) << 1) | gpio_get_level(ADDR0_PIN));
    s_rec   = s_auto;
    ESP_LOGI(TAG, "mode1=%s mode2=%s addr=%u", s_auto ? "auto-record" : "manual", s_files ? "files" : "serial", s_addr);

    /* The button pulls MODE1 to ground. In manual mode the switch holds it low, so no edge comes. */
    const gpio_config_t bc = { .pin_bit_mask = 1ULL << MODE1_PIN, .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&bc);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(MODE1_PIN, button_isr, NULL);
}

bool io_auto_mode(void)      { return s_auto; }
bool io_usb_files_mode(void) { return s_files; }
uint8_t io_addr(void)        { return s_addr; }
bool rec_get(void)           { return s_rec; }
void rec_set(bool on)        { s_rec = on; }
