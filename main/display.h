#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define DISPLAY_COLS 20
#define DISPLAY_ROWS 10

esp_err_t display_init(void);
bool display_ok(void);
// Redraws only rows whose text changed. lines[i] may be NULL for blank.
void display_lines(const char *const lines[DISPLAY_ROWS]);
