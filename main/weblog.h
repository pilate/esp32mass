#pragma once
#include <stddef.h>
// Captures ESP_LOG output into a RAM ring buffer so it can be read over HTTP,
// since the USB port is busy being a disk and UART0 is on the QWIIC header.
void weblog_init(void);
size_t weblog_copy(char *dst, size_t cap);   // newest bytes, oldest first
