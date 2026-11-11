// LilyGO T-Dongle-S3 pin map (ESP32-S3, 16 MB flash, no PSRAM)
#pragma once

// MicroSD, 4-bit SDMMC
#define BOARD_SD_CLK        12
#define BOARD_SD_CMD        16
#define BOARD_SD_D0         14
#define BOARD_SD_D1         17
#define BOARD_SD_D2         21
#define BOARD_SD_D3         18
#define BOARD_SD_FREQ_KHZ   20000   // SDMMC_FREQ_DEFAULT; raise to 40000 once wiring is proven

// APA102 RGB LED (bit-banged)
#define BOARD_LED_DATA      40
#define BOARD_LED_CLK       39

// BOOT button, active low
#define BOARD_BUTTON        0

// ST7735 0.96" 80x160 LCD on SPI2
#define BOARD_LCD_MOSI      3
#define BOARD_LCD_SCLK      5
#define BOARD_LCD_CS        4
#define BOARD_LCD_DC        2
#define BOARD_LCD_RST       1
#define BOARD_LCD_BL        38      // backlight, active LOW
#define BOARD_LCD_W         160
#define BOARD_LCD_H         80
// Panel offsets for this 80x160 ST7735 glass in landscape. Verify on hardware.
#define BOARD_LCD_GAP_X     1
#define BOARD_LCD_GAP_Y     26
