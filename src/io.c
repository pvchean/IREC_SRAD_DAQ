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
static volatile bool s_rec_locked, s_isr_ready;
/* One-time "serial mode" request over a software reset (files_mode_leave). RTC no-init RAM keeps its value over esp_restart() and
   has random content after a power-up, so the flag is two words that must match: magic and its complement. */
#define LEAVE_MAGIC 0x53455249U         /* "SERI" */
static RTC_NOINIT_ATTR uint32_t s_leave_a, s_leave_b;
static uint8_t s_addr;
static volatile bool s_rec;
static volatile int64_t s_last_us;

static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    (void)arg;
    if (s_auto && now - s_last_us > DEBOUNCE_US) {   /* manual mode: only the REC command starts and stops */
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
    if (s_leave_a == LEAVE_MAGIC && s_leave_b == ~LEAVE_MAGIC) {      /* files_mode_leave() asked for serial mode, once */
        ESP_LOGW(TAG, "files mode skipped once: serial mode");
        s_files = false;
    }
    s_leave_a = 0U; s_leave_b = 0U;
    s_addr  = (uint8_t)((gpio_get_level(ADDR2_PIN) << 2) | (gpio_get_level(ADDR1_PIN) << 1) | gpio_get_level(ADDR0_PIN));
    s_rec   = s_auto;
    ESP_LOGI(TAG, "mode1=%s mode2=%s addr=%u", s_auto ? "auto-record" : "manual", s_files ? "files" : "serial", s_addr);
#ifdef DAQ_MTP
    io_rec_disable();                   /* this build never records: no auto record, no button (the ISR is removed below) */
    ESP_LOGW(TAG, "daq_mtp build: recording is disabled");
#endif

    /* The button pulls MODE1 to ground. It works in auto mode only; in manual mode the switch holds the
       line low, and the REC ON / REC OFF command controls the recording. */
    const gpio_config_t bc = { .pin_bit_mask = 1ULL << MODE1_PIN, .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&bc);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(MODE1_PIN, button_isr, NULL);
    s_isr_ready = true;
    if (s_rec_locked) { gpio_isr_handler_remove(MODE1_PIN); }
}

void io_rec_disable(void)
{
    s_rec_locked = true;
    s_auto = false;                     /* the button ISR does nothing, and the AUTOREC LED is off */
    s_rec = false;
    if (s_isr_ready) { gpio_isr_handler_remove(MODE1_PIN); }
}

bool io_auto_mode(void)      { return s_auto; }
bool io_usb_files_mode(void) { return s_files; }
uint8_t io_addr(void)        { return s_addr; }
bool rec_get(void)           { return s_rec; }
void rec_set(bool on)        { if (!on || !s_rec_locked) { s_rec = on; } }
bool rec_locked(void)        { return s_rec_locked; }

void io_request_serial_once(void) { s_leave_a = LEAVE_MAGIC; s_leave_b = ~LEAVE_MAGIC; }
