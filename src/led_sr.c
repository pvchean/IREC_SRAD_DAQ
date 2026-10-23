#include "led_sr.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include "board.h"

static uint8_t s_byte = 0xFF;       /* 0 = LED on */

static void push(void)
{
    gpio_set_level(SR_LATCH_PIN, 0);
    for (int b = 7; b >= 0; b--) {              /* MSB first, as shiftOut(MSBFIRST) */
        gpio_set_level(SR_DATA_PIN, (s_byte >> b) & 1);
        gpio_set_level(SR_CLOCK_PIN, 1);
        esp_rom_delay_us(1);
        gpio_set_level(SR_CLOCK_PIN, 0);
    }
    gpio_set_level(SR_LATCH_PIN, 1);
}

void led_sr_init(void)
{
    const gpio_config_t gc = {
        .pin_bit_mask = (1ULL << SR_LATCH_PIN) | (1ULL << SR_CLOCK_PIN) | (1ULL << SR_DATA_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&gc);
    push();
}

void led_set(int led, bool on)
{
    if (led < 0 || led > 7) { return; }
    uint8_t n = on ? (uint8_t)(s_byte & ~(1U << led)) : (uint8_t)(s_byte | (1U << led));
    if (n == s_byte) { return; }
    s_byte = n;
    push();
}
