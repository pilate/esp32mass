#pragma once
#include <stdint.h>
void status_led_init(void);
// brightness 0..31 (APA102 global brightness)
void status_led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
