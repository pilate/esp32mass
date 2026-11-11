#include "status_led.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static inline void clk_pulse(void)
{
    gpio_set_level(BOARD_LED_CLK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(BOARD_LED_CLK, 0);
    esp_rom_delay_us(1);
}

static void send_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(BOARD_LED_DATA, (b >> i) & 1);
        clk_pulse();
    }
}

void status_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_LED_DATA) | (1ULL << BOARD_LED_CLK),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_LED_CLK, 0);
    status_led_set(0, 0, 0, 0);
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    for (int i = 0; i < 4; i++) {
        send_byte(0x00);               // start frame
    }
    send_byte(0xE0 | (brightness & 0x1F));
    send_byte(b);                      // APA102 frame order is B, G, R
    send_byte(g);
    send_byte(r);
    for (int i = 0; i < 4; i++) {
        send_byte(0xFF);               // end frame
    }
}
