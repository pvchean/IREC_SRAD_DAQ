#include "led_sr.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "board.h"

#define FRAME_US 4000U                  /* 250 Hz: no visible flicker */
#define MIN_PHASE_US 20U

static volatile uint8_t s_state = 0xFF; /* 0 = LED on. led_set() writes it; the timer shows it. */
static volatile uint8_t s_duty = LED_DEFAULT_BRIGHTNESS;
static esp_timer_handle_t s_timer;
static bool s_lit_phase;                /* the next callback ends the lit phase */

static void push(uint8_t v)
{
    gpio_set_level(SR_LATCH_PIN, 0);
    for (int b = 7; b >= 0; b--) {              /* MSB first, as shiftOut(MSBFIRST) */
        gpio_set_level(SR_DATA_PIN, (v >> b) & 1);
        gpio_set_level(SR_CLOCK_PIN, 1);
        esp_rom_delay_us(1);
        gpio_set_level(SR_CLOCK_PIN, 0);
    }
    gpio_set_level(SR_LATCH_PIN, 1);
}

static void frame_cb(void *arg)
{
    uint32_t duty = s_duty, on_us;
    (void)arg;
    if (duty == 0U)        { push(0xFF); esp_timer_start_once(s_timer, FRAME_US); s_lit_phase = false; return; }
    if (duty >= 255U)      { push(s_state); esp_timer_start_once(s_timer, FRAME_US); s_lit_phase = false; return; }
    on_us = FRAME_US * duty / 255U;
    if (on_us < MIN_PHASE_US) { on_us = MIN_PHASE_US; }
    if (!s_lit_phase) {                         /* start of a frame: LEDs on */
        push(s_state);
        s_lit_phase = true;
        esp_timer_start_once(s_timer, on_us);
    } else {                                    /* LEDs off for the rest of the frame */
        push(0xFF);
        s_lit_phase = false;
        esp_timer_start_once(s_timer, FRAME_US - on_us);
    }
}

void led_sr_init(void)
{
    const gpio_config_t gc = {
        .pin_bit_mask = (1ULL << SR_LATCH_PIN) | (1ULL << SR_CLOCK_PIN) | (1ULL << SR_DATA_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    const esp_timer_create_args_t ta = { .callback = frame_cb, .name = "led_pwm" };
    gpio_config(&gc);
    push(0xFF);
    esp_timer_create(&ta, &s_timer);
    esp_timer_start_once(s_timer, FRAME_US);
}

void led_set(int led, bool on)
{
    if (led < 0 || led > 7) { return; }
    s_state = on ? (uint8_t)(s_state & ~(1U << led)) : (uint8_t)(s_state | (1U << led));
}

void led_set_brightness(uint8_t duty) { s_duty = duty; }
uint8_t led_get_brightness(void)      { return s_duty; }
