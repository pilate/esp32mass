#include <string.h>
#include "display.h"
#include "board.h"
#include "font8x8_basic.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static bool s_ok;
static char s_shadow[DISPLAY_ROWS][DISPLAY_COLS + 1];
// esp_lcd queues the pixel transfer and only waits for it at the start of the
// next draw call, so a row buffer must not be touched while its DMA may still be
// reading it. Alternating two buffers guarantees the one being refilled is idle.
static uint16_t s_rowbuf[2][BOARD_LCD_W * 8];
static int s_rowbuf_next;

// RGB565 with bytes pre-swapped for the SPI panel (MSB first on the wire).
#define RGB(r, g, b) ((uint16_t)__builtin_bswap16((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BG      RGB(0, 0, 0)
#define COL_FG      RGB(220, 220, 220)
#define COL_TITLE   RGB(80, 200, 255)

esp_err_t display_init(void)
{
    gpio_config_t bl = { .pin_bit_mask = 1ULL << BOARD_LCD_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(BOARD_LCD_BL, 1);   // off while initialising (active low)

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_rowbuf[0]),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    // ST7735 and ST7789 share SLPOUT/MADCTL/COLMOD/CASET/RASET/RAMWR, which is all we use.
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    // Clear. Every row sends the same unchanging buffer, so reusing it is safe here.
    for (int i = 0; i < BOARD_LCD_W * 8; i++) {
        s_rowbuf[0][i] = COL_BG;
    }
    for (int y = 0; y < BOARD_LCD_H; y += 8) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_LCD_W, y + 8, s_rowbuf[0]);
    }
    s_rowbuf_next = 1;   // buffer 0 may still be in flight
    memset(s_shadow, 0, sizeof(s_shadow));
    gpio_set_level(BOARD_LCD_BL, 0);   // backlight on
    s_ok = true;
    ESP_LOGI(TAG, "ST7735 %dx%d ready", BOARD_LCD_W, BOARD_LCD_H);
    return ESP_OK;
}

bool display_ok(void)
{
    return s_ok;
}

static void draw_row(int row, const char *text, uint16_t fg)
{
    uint16_t *buf = s_rowbuf[s_rowbuf_next];
    s_rowbuf_next ^= 1;
    for (int i = 0; i < BOARD_LCD_W * 8; i++) {
        buf[i] = COL_BG;
    }
    size_t len = text ? strlen(text) : 0;
    if (len > DISPLAY_COLS) {
        len = DISPLAY_COLS;
    }
    for (size_t ci = 0; ci < len; ci++) {
        unsigned char c = (unsigned char)text[ci];
        if (c < 32 || c > 126) {
            c = '?';
        }
        const unsigned char *glyph = font8x8_basic[c];
        for (int y = 0; y < 8; y++) {
            unsigned char bits = glyph[y];
            uint16_t *dst = &buf[y * BOARD_LCD_W + ci * 8];
            for (int x = 0; x < 8; x++) {
                if (bits & (1 << x)) {
                    dst[x] = fg;
                }
            }
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, row * 8, BOARD_LCD_W, row * 8 + 8, buf);
}

void display_lines(const char *const lines[DISPLAY_ROWS])
{
    if (!s_ok) {
        return;
    }
    for (int r = 0; r < DISPLAY_ROWS; r++) {
        const char *t = lines[r] ? lines[r] : "";
        if (strncmp(t, s_shadow[r], DISPLAY_COLS) == 0) {
            continue;
        }
        strlcpy(s_shadow[r], t, sizeof(s_shadow[r]));
        draw_row(r, t, r == 0 ? COL_TITLE : COL_FG);
    }
}
