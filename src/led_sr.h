/* LED shift register (active low). Port of led_sr_driver.cpp without the dimming: a static on/off
   state is simpler and uses no CPU between changes. */
#ifndef LED_SR_H
#define LED_SR_H

#include <stdbool.h>

void led_sr_init(void);
void led_set(int led, bool on);     /* led 0..7; call from one task only */

#endif
