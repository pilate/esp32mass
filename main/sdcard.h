#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Raw block access to the microSD card. No filesystem is ever mounted.
esp_err_t sdcard_init(void);
bool sdcard_try_mount(void);          // (re)probe the slot; true if a card is usable
bool sdcard_present(void);
uint32_t sdcard_sector_count(void);
uint32_t sdcard_sector_size(void);
uint64_t sdcard_capacity_bytes(void);
esp_err_t sdcard_read(uint32_t lba, void *dst, uint32_t sectors);
esp_err_t sdcard_write(uint32_t lba, const void *src, uint32_t sectors);
void sdcard_get_info(char *name, size_t name_len, uint32_t *freq_khz, uint32_t *errors);
