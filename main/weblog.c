#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "weblog.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define WEBLOG_SIZE 8192

static char s_ring[WEBLOG_SIZE];
static size_t s_head;
static size_t s_len;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static int weblog_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        portENTER_CRITICAL(&s_mux);
        for (size_t i = 0; i < len; i++) {
            s_ring[s_head] = line[i];
            s_head = (s_head + 1) % WEBLOG_SIZE;
        }
        if (s_len + len > WEBLOG_SIZE) {
            s_len = WEBLOG_SIZE;
        } else {
            s_len += len;
        }
        portEXIT_CRITICAL(&s_mux);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void weblog_init(void)
{
    s_prev = esp_log_set_vprintf(weblog_vprintf);
}

size_t weblog_copy(char *dst, size_t cap)
{
    portENTER_CRITICAL(&s_mux);
    size_t n = s_len < cap ? s_len : cap;
    size_t start = (s_head + WEBLOG_SIZE - n) % WEBLOG_SIZE;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s_ring[(start + i) % WEBLOG_SIZE];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}
