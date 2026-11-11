#include <stdio.h>
#include <string.h>
#include "board.h"
#include "settings.h"
#include "sdcard.h"
#include "usb_msc.h"
#include "wifi_mgr.h"
#include "web.h"
#include "upload.h"
#include "status_led.h"
#include "display.h"
#include "weblog.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define TICK_MS            50
#define BUTTON_LONG_MS     5000

static void format_size(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= (1ULL << 30)) {
        snprintf(out, cap, "%.1fG", bytes / (double)(1ULL << 30));
    } else if (bytes >= (1ULL << 20)) {
        snprintf(out, cap, "%.0fM", bytes / (double)(1ULL << 20));
    } else {
        snprintf(out, cap, "%lluK", (unsigned long long)(bytes >> 10));
    }
}

static void update_led(uint32_t phase, bool button_long)
{
    bool blink = (phase / 5) % 2;
    uint8_t tri = phase % 20;
    uint8_t breathe = tri < 10 ? 2 + tri * 2 : 2 + (19 - tri) * 2;
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    usb_msc_stats_t st;
    usb_msc_get_stats(&st);

    if (button_long) {
        status_led_set(255, 0, 0, 20);
    } else if (upload_active()) {
        status_led_set(180, 0, 255, breathe);
    } else if (!sdcard_present()) {
        status_led_set(255, 0, 0, blink ? 10 : 0);
    } else if (!ws.sta_connected && ws.ap_active) {
        status_led_set(0, 60, 255, blink ? 10 : 1);
    } else if (!ws.sta_connected && ws.sta_configured) {
        status_led_set(255, 160, 0, blink ? 8 : 1);
    } else if (usb_msc_host_mounted()) {
        bool busy = st.last_io_us && (esp_timer_get_time() - st.last_io_us) < 200000;
        status_led_set(0, 255, 40, busy ? 24 : 5);
    } else if (usb_msc_attached()) {
        status_led_set(0, 255, 40, 2);
    } else {
        status_led_set(255, 120, 0, 2);
    }
}

// The screen is 20 characters wide; truncation of longer strings is intended.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void update_display(void)
{
    if (!display_ok()) {
        return;
    }
    static char l[DISPLAY_ROWS][DISPLAY_COLS + 1];
    const char *lines[DISPLAY_ROWS];
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    const image_meta_t *m = image_meta_get();
    char sz[16];

    snprintf(l[0], sizeof(l[0]), "esp32mass %s", esp_app_get_description()->version);
    if (ws.sta_connected) {
        snprintf(l[1], sizeof(l[1]), "WiFi %.14s", ws.sta_ssid);
        snprintf(l[2], sizeof(l[2]), "IP %s", ws.sta_ip);
    } else if (ws.sta_configured) {
        snprintf(l[1], sizeof(l[1]), "WiFi %.14s", ws.sta_ssid);
        snprintf(l[2], sizeof(l[2]), "%s", ws.sta_connecting ? "connecting..." : "not connected");
    } else {
        snprintf(l[1], sizeof(l[1]), "WiFi not set up");
        snprintf(l[2], sizeof(l[2]), "join AP below");
    }
    if (ws.ap_active) {
        snprintf(l[3], sizeof(l[3]), "AP %s", ws.ap_ssid);
        snprintf(l[4], sizeof(l[4]), "   %s", ws.ap_ip);
    } else {
        snprintf(l[3], sizeof(l[3]), "http://%s.local", ws.hostname);
        l[4][0] = 0;
    }
    if (!sdcard_present()) {
        snprintf(l[5], sizeof(l[5]), "SD: no card");
    } else {
        format_size(sdcard_capacity_bytes(), sz, sizeof(sz));
        snprintf(l[5], sizeof(l[5]), "SD: %s %s", sz, settings_get()->read_only ? "RO" : "RW");
    }
    if (upload_active()) {
        cJSON *tmp = cJSON_CreateObject();
        upload_status_json(tmp);
        double size = cJSON_GetObjectItem(tmp, "size")->valuedouble;
        double recv = cJSON_GetObjectItem(tmp, "received")->valuedouble;
        bool verifying = cJSON_IsTrue(cJSON_GetObjectItem(tmp, "verifying"));
        double rate = cJSON_GetObjectItem(tmp, "rate_bps")->valuedouble;
        cJSON_Delete(tmp);
        snprintf(l[6], sizeof(l[6]), "USB: detached");
        snprintf(l[7], sizeof(l[7]), verifying ? "Verifying..." : "Uploading %3.0f%%", size > 0 ? recv * 100.0 / size : 0);
        snprintf(l[8], sizeof(l[8]), "%.2f MB/s", rate / 1048576.0);
    } else {
        if (usb_msc_host_mounted()) {
            snprintf(l[6], sizeof(l[6]), "USB: host connected");
        } else if (usb_msc_attached()) {
            snprintf(l[6], sizeof(l[6]), usb_msc_ejected_by_host() ? "USB: ejected" : "USB: attached");
        } else {
            snprintf(l[6], sizeof(l[6]), "USB: detached");
        }
        if (m->size) {
            format_size(m->size, sz, sizeof(sz));
            snprintf(l[7], sizeof(l[7]), "Img %.16s", m->name);
            snprintf(l[8], sizeof(l[8]), "    %s", sz);
        } else {
            snprintf(l[7], sizeof(l[7]), "Img: none recorded");
            l[8][0] = 0;
        }
    }
    snprintf(l[9], sizeof(l[9]), "heap %luK", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    for (int i = 0; i < DISPLAY_ROWS; i++) {
        lines[i] = l[i];
    }
    display_lines(lines);
}
#pragma GCC diagnostic pop

void app_main(void)
{
    weblog_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_init();

    status_led_init();
    status_led_set(0, 0, 255, 6);
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "display unavailable");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "esp32mass %s (%s %s), free heap %u", app->version, app->date, app->time,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (sdcard_init() != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed");
    }
    if (usb_msc_init() == ESP_OK) {
        if (settings_get()->auto_attach && sdcard_present()) {
            usb_msc_set_attached(true);
        }
    } else {
        ESP_LOGE(TAG, "USB init failed");
    }

    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(upload_init());
    ESP_ERROR_CHECK(web_start());
    ESP_LOGI(TAG, "ready, free heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    uint32_t tick = 0;
    int64_t press_start = 0;
    bool long_done = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        tick++;
        int64_t now = esp_timer_get_time();

        // Button: short press toggles USB attach, hold 5 s forgets Wi-Fi and reboots.
        bool pressed = gpio_get_level(BOARD_BUTTON) == 0;
        bool holding_long = false;
        if (pressed) {
            if (!press_start) {
                press_start = now;
                long_done = false;
            }
            if (!long_done && now - press_start > BUTTON_LONG_MS * 1000LL) {
                long_done = true;
                ESP_LOGW(TAG, "button held: clearing Wi-Fi credentials and rebooting");
                wifi_mgr_forget();
                status_led_set(255, 0, 0, 31);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            holding_long = now - press_start > 1500 * 1000LL;
        } else if (press_start) {
            int64_t held = now - press_start;
            press_start = 0;
            if (held > 40 * 1000LL && held < 1500 * 1000LL && !upload_active()) {
                if (usb_msc_attached()) {
                    usb_msc_set_attached(false);
                } else if (sdcard_try_mount()) {
                    usb_msc_set_attached(true);
                }
            }
        }

        if (tick % 2 == 0) {
            update_led(tick / 2, holding_long);
        }
        if (tick % 20 == 0) {
            if (!sdcard_present() && !upload_active()) {
                if (sdcard_try_mount() && settings_get()->auto_attach && !usb_msc_attached()) {
                    usb_msc_set_attached(true);
                }
            }
            upload_check_stale();
            update_display();
        }
    }
}
