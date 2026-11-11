#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Raw block access to the microSD card. No filesystem is ever mounted.

// The SD driver reports min(READ_BL_LEN, 512) and real cards never go below 512,
// so the sector size is fixed; sdcard_try_mount() rejects any card that disagrees.
#define SDCARD_SECTOR_SIZE 512

esp_err_t sdcard_init(void);
bool sdcard_try_mount(void);          // (re)probe the slot; true if a card is usable
bool sdcard_present(void);
uint32_t sdcard_sector_count(void);
uint64_t sdcard_capacity_bytes(void);
esp_err_t sdcard_read(uint32_t lba, void *dst, uint32_t sectors);
esp_err_t sdcard_write(uint32_t lba, const void *src, uint32_t sectors);
void sdcard_get_info(char *name, size_t name_len, uint32_t *freq_khz, uint32_t *errors);
