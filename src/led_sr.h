/* LED shift register (active low). Port of led_sr_driver.cpp. The sketch used binary angle modulation
   with a 32 us timer step per bit, about 8000 timer events per second. Here all LEDs share one PWM duty at
   250 Hz: two timer events per frame, about 500 per second. */
#ifndef LED_SR_H
#define LED_SR_H

#include <stdbool.h>
#include <stdint.h>

#define LED_DEFAULT_BRIGHTNESS 48U       /* 0 = off, 255 = full */

void led_sr_init(void);
void led_set(int led, bool on);          /* led 0..7 */
void led_set_brightness(uint8_t duty);   /* 0..255 for all LEDs */
uint8_t led_get_brightness(void);

#endif
