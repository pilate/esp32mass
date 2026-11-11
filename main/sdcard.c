#include <string.h>
#include "sdcard.h"
#include "board.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

static const char *TAG = "sdcard";

static sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static sdmmc_card_t s_card;
static SemaphoreHandle_t s_lock;
static bool s_host_ready;
static volatile bool s_present;
static uint32_t s_errors;
static uint32_t s_consecutive_errors;
static int64_t s_last_probe_log;

esp_err_t sdcard_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_host.max_freq_khz = BOARD_SD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.d1 = BOARD_SD_D1;
    slot.d2 = BOARD_SD_D2;
    slot.d3 = BOARD_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init failed");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(s_host.slot, &slot), TAG, "slot init failed");
    s_host_ready = true;
    sdcard_try_mount();
    return ESP_OK;
}

bool sdcard_try_mount(void)
{
    if (s_present) {
        return true;
    }
    if (!s_host_ready) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_card, 0, sizeof(s_card));
    esp_err_t err = sdmmc_card_init(&s_host, &s_card);
    if (err == ESP_OK) {
        s_present = true;
        s_consecutive_errors = 0;
        char name[8] = {0};
        memcpy(name, s_card.cid.name, sizeof(s_card.cid.name) < 7 ? sizeof(s_card.cid.name) : 7);
        ESP_LOGI(TAG, "card '%s': %llu MB, %lu x %lu B sectors, %lu kHz, %d-bit",
                 name,
                 (unsigned long long)((uint64_t)s_card.csd.capacity * s_card.csd.sector_size / (1024 * 1024)),
                 (unsigned long)s_card.csd.capacity, (unsigned long)s_card.csd.sector_size,
                 (unsigned long)s_card.real_freq_khz, s_card.log_bus_width ? (1 << s_card.log_bus_width) : 1);
    } else {
        int64_t now = esp_timer_get_time();
        if (now - s_last_probe_log > 30 * 1000000LL) {
            s_last_probe_log = now;
            ESP_LOGW(TAG, "no card (%s)", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return s_present;
}

bool sdcard_present(void)
{
    return s_present;
}

uint32_t sdcard_sector_count(void)
{
    return s_present ? s_card.csd.capacity : 0;
}

uint32_t sdcard_sector_size(void)
{
    return s_present ? s_card.csd.sector_size : 512;
}

uint64_t sdcard_capacity_bytes(void)
{
    return (uint64_t)sdcard_sector_count() * sdcard_sector_size();
}

static void note_error(esp_err_t err, const char *op, uint32_t lba, uint32_t n)
{
    s_errors++;
    s_consecutive_errors++;
    ESP_LOGE(TAG, "%s lba=%lu n=%lu failed: %s", op, (unsigned long)lba, (unsigned long)n, esp_err_to_name(err));
    if (s_consecutive_errors >= 3) {
        ESP_LOGW(TAG, "repeated errors, treating card as removed");
        s_present = false;
    }
}

esp_err_t sdcard_read(uint32_t lba, void *dst, uint32_t sectors)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lba + sectors > s_card.csd.capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = sdmmc_read_sectors(&s_card, dst, lba, sectors);
    if (err != ESP_OK) {
        note_error(err, "read", lba, sectors);
    } else {
        s_consecutive_errors = 0;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sdcard_write(uint32_t lba, const void *src, uint32_t sectors)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lba + sectors > s_card.csd.capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = sdmmc_write_sectors(&s_card, src, lba, sectors);
    if (err != ESP_OK) {
        note_error(err, "write", lba, sectors);
    } else {
        s_consecutive_errors = 0;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void sdcard_get_info(char *name, size_t name_len, uint32_t *freq_khz, uint32_t *errors)
{
    if (name && name_len) {
        name[0] = 0;
        if (s_present) {
            size_t n = sizeof(s_card.cid.name);
            if (n > name_len - 1) {
                n = name_len - 1;
            }
            memcpy(name, s_card.cid.name, n);
            name[n] = 0;
        }
    }
    if (freq_khz) {
        *freq_khz = s_present ? s_card.real_freq_khz : 0;
    }
    if (errors) {
        *errors = s_errors;
    }
}
