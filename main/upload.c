// Image upload pipeline: HTTP body -> ping-pong DMA buffers -> SD writer task.
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "upload.h"
#include "web.h"
#include "sdcard.h"
#include "usb_msc.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "upload";

#define UP_BUF_SECTORS     64            // 32 KB per buffer
#define UP_BUF_COUNT       2
#define UP_STALE_US        (120 * 1000000LL)
#define UP_CHUNK_HINT      (1024 * 1024)   // what the browser should send per request

typedef struct {
    int idx;
    uint32_t lba;
    uint32_t sectors;
} up_job_t;

static struct {
    bool active;
    char name[96];
    uint64_t size;
    uint64_t dispatched;            // bytes handed to the writer (sector aligned)
    volatile uint64_t written;      // bytes confirmed on the card
    int cur_idx;                    // buffer being filled, -1 if none
    size_t cur_fill;
    mbedtls_sha256_context sha;     // running hash of received bytes
    mbedtls_sha256_context sha_at_dispatch;
    int64_t started_us;
    int64_t last_activity_us;
    volatile esp_err_t write_err;
    char error[96];
    volatile bool verifying;
    volatile uint64_t verify_pos;
    char result_sha[65];
    bool sha_inited;
} s_up;

static uint8_t *s_buf[UP_BUF_COUNT];
static size_t s_buf_bytes;
static QueueHandle_t s_free_q;
static QueueHandle_t s_full_q;
static SemaphoreHandle_t s_lock;

static void writer_task(void *arg)
{
    (void)arg;
    up_job_t j;
    while (1) {
        xQueueReceive(s_full_q, &j, portMAX_DELAY);
        if (j.sectors) {
            esp_err_t e = sdcard_write(j.lba, s_buf[j.idx], j.sectors);
            if (e != ESP_OK) {
                if (s_up.write_err == ESP_OK) {
                    s_up.write_err = e;
                }
            } else {
                s_up.written += (uint64_t)j.sectors * SDCARD_SECTOR_SIZE;
            }
        }
        xQueueSend(s_free_q, &j.idx, portMAX_DELAY);
    }
}

esp_err_t upload_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_free_q = xQueueCreate(UP_BUF_COUNT, sizeof(int));
    s_full_q = xQueueCreate(UP_BUF_COUNT, sizeof(up_job_t));
    if (!s_lock || !s_free_q || !s_full_q) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(writer_task, "sd_writer", 4096, NULL, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_up.cur_idx = -1;
    return ESP_OK;
}

bool upload_active(void)
{
    return s_up.active;
}

static void drain(void)
{
    // Wait until every buffer not currently being filled is back on the free queue.
    int expected = UP_BUF_COUNT - (s_up.cur_idx >= 0 ? 1 : 0);
    while ((int)uxQueueMessagesWaiting(s_free_q) < expected) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void free_buffers(void)
{
    xQueueReset(s_free_q);
    for (int i = 0; i < UP_BUF_COUNT; i++) {
        if (s_buf[i]) {
            heap_caps_free(s_buf[i]);
            s_buf[i] = NULL;
        }
    }
    s_buf_bytes = 0;
}

static bool alloc_buffers(void)
{
    size_t sectors = UP_BUF_SECTORS;
    while (sectors >= 8) {
        size_t bytes = sectors * SDCARD_SECTOR_SIZE;
        bool ok = true;
        for (int i = 0; i < UP_BUF_COUNT; i++) {
            s_buf[i] = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!s_buf[i]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            s_buf_bytes = bytes;
            xQueueReset(s_free_q);
            for (int i = 0; i < UP_BUF_COUNT; i++) {
                xQueueSend(s_free_q, &i, 0);
            }
            ESP_LOGI(TAG, "buffers: %d x %u bytes", UP_BUF_COUNT, (unsigned)bytes);
            return true;
        }
        free_buffers();
        sectors /= 2;
    }
    return false;
}

static void sha_reset(void)
{
    if (s_up.sha_inited) {
        mbedtls_sha256_free(&s_up.sha);
        mbedtls_sha256_free(&s_up.sha_at_dispatch);
    }
    mbedtls_sha256_init(&s_up.sha);
    mbedtls_sha256_init(&s_up.sha_at_dispatch);
    mbedtls_sha256_starts(&s_up.sha, 0);
    mbedtls_sha256_clone(&s_up.sha_at_dispatch, &s_up.sha);
    s_up.sha_inited = true;
}

static void dispatch_current(void)
{
    // Pads the partially filled buffer with zeros up to a sector boundary.
    size_t fill = s_up.cur_fill;
    size_t sectors = (fill + SDCARD_SECTOR_SIZE - 1) / SDCARD_SECTOR_SIZE;
    if (sectors * SDCARD_SECTOR_SIZE > fill) {
        memset(s_buf[s_up.cur_idx] + fill, 0, sectors * SDCARD_SECTOR_SIZE - fill);
    }
    up_job_t j = { .idx = s_up.cur_idx, .lba = (uint32_t)(s_up.dispatched / SDCARD_SECTOR_SIZE), .sectors = (uint32_t)sectors };
    xQueueSend(s_full_q, &j, portMAX_DELAY);
    s_up.dispatched += fill;
    s_up.cur_idx = -1;
    s_up.cur_fill = 0;
    mbedtls_sha256_clone(&s_up.sha_at_dispatch, &s_up.sha);
}

static void discard_partial(void)
{
    if (s_up.cur_idx >= 0) {
        xQueueSend(s_free_q, &s_up.cur_idx, portMAX_DELAY);
        s_up.cur_idx = -1;
        s_up.cur_fill = 0;
        mbedtls_sha256_clone(&s_up.sha, &s_up.sha_at_dispatch);
    }
}

static void end_upload(bool reattach)
{
    drain();
    discard_partial();
    free_buffers();
    s_up.active = false;
    s_up.verifying = false;
    if (reattach && settings_get()->auto_attach && sdcard_present()) {
        usb_msc_set_attached(true);
    }
}

static void fail_upload(const char *msg)
{
    strlcpy(s_up.error, msg, sizeof(s_up.error));
    ESP_LOGE(TAG, "upload failed: %s", msg);
    end_upload(false);
}

void upload_check_stale(void)
{
    if (!s_up.active) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;   // a handler is busy, so it is not stale
    }
    if (esp_timer_get_time() - s_up.last_activity_us > UP_STALE_US) {
        fail_upload("upload timed out waiting for data");
    }
    xSemaphoreGive(s_lock);
}

void upload_get_progress(upload_progress_t *out)
{
    out->active = s_up.active;
    out->verifying = s_up.verifying;
    out->size = s_up.size;
    out->received = s_up.dispatched + s_up.cur_fill;
    double secs = (esp_timer_get_time() - s_up.started_us) / 1e6;
    out->rate_bps = (s_up.active && secs > 0.5) ? out->received / secs : 0;
}

void upload_status_json(cJSON *o)
{
    upload_progress_t p;
    upload_get_progress(&p);
    cJSON_AddBoolToObject(o, "active", p.active);
    cJSON_AddStringToObject(o, "name", s_up.name);
    cJSON_AddNumberToObject(o, "size", (double)p.size);
    cJSON_AddNumberToObject(o, "received", (double)p.received);
    cJSON_AddNumberToObject(o, "written", (double)s_up.written);
    cJSON_AddStringToObject(o, "error", s_up.error);
    cJSON_AddBoolToObject(o, "verifying", p.verifying);
    cJSON_AddNumberToObject(o, "verify_pos", (double)s_up.verify_pos);
    cJSON_AddStringToObject(o, "sha256", s_up.result_sha);
    cJSON_AddNumberToObject(o, "rate_bps", p.rate_bps);
    cJSON_AddNumberToObject(o, "chunk_size", UP_CHUNK_HINT);
}

esp_err_t upload_begin_handler(httpd_req_t *req)
{
    cJSON *body = web_read_json_body(req);
    if (!body) {
        return web_send_error(req, "400 Bad Request", "invalid JSON");
    }
    cJSON *jname = cJSON_GetObjectItem(body, "name");
    cJSON *jsize = cJSON_GetObjectItem(body, "size");
    char name[96] = "image.img";
    if (cJSON_IsString(jname) && jname->valuestring[0]) {
        strlcpy(name, jname->valuestring, sizeof(name));
    }
    uint64_t size = cJSON_IsNumber(jsize) ? (uint64_t)jsize->valuedouble : 0;
    cJSON_Delete(body);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_up.active) {
        xSemaphoreGive(s_lock);
        return web_send_error(req, "409 Conflict", "an upload is already in progress");
    }
    if (!sdcard_try_mount()) {
        xSemaphoreGive(s_lock);
        return web_send_error(req, "409 Conflict", "no SD card");
    }
    if (size == 0 || size > sdcard_capacity_bytes()) {
        xSemaphoreGive(s_lock);
        return web_send_error(req, "400 Bad Request", "image size is zero or larger than the card");
    }

    usb_msc_set_attached(false);
    if (!alloc_buffers()) {
        if (settings_get()->auto_attach) {
            usb_msc_set_attached(true);
        }
        xSemaphoreGive(s_lock);
        return web_send_error(req, "500 Internal Server Error", "out of memory for buffers");
    }
    // The card is about to be overwritten, so the recorded image no longer describes
    // it. Clearing now (rather than only on abort) keeps a failed or timed-out upload
    // from leaving the old name and SHA-256 attached to a half-written card.
    image_meta_clear();
    memset(s_up.name, 0, sizeof(s_up.name));
    strlcpy(s_up.name, name, sizeof(s_up.name));
    s_up.size = size;
    s_up.dispatched = 0;
    s_up.written = 0;
    s_up.cur_idx = -1;
    s_up.cur_fill = 0;
    s_up.write_err = ESP_OK;
    s_up.error[0] = 0;
    s_up.result_sha[0] = 0;
    s_up.verifying = false;
    s_up.verify_pos = 0;
    s_up.started_us = esp_timer_get_time();
    s_up.last_activity_us = s_up.started_us;
    sha_reset();
    s_up.active = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "begin '%s', %llu bytes", name, (unsigned long long)size);
    cJSON *o = web_json_ok();
    cJSON_AddNumberToObject(o, "chunk_size", UP_CHUNK_HINT);
    cJSON_AddNumberToObject(o, "buffer", (double)s_buf_bytes);
    return web_send_json(req, o);
}

esp_err_t upload_chunk_handler(httpd_req_t *req)
{
    char query[64];
    char val[24];
    uint64_t offset = UINT64_MAX;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "offset", val, sizeof(val)) == ESP_OK) {
        offset = strtoull(val, NULL, 10);
    }
    if (offset == UINT64_MAX) {
        return web_send_error(req, "400 Bad Request", "missing offset");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_up.active) {
        xSemaphoreGive(s_lock);
        return web_send_error(req, "409 Conflict", "no upload in progress");
    }
    uint64_t expected = s_up.dispatched + s_up.cur_fill;
    if (offset != expected) {
        xSemaphoreGive(s_lock);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "offset mismatch");
        cJSON_AddNumberToObject(o, "received", (double)expected);
        httpd_resp_set_status(req, "409 Conflict");
        return web_send_json(req, o);
    }
    size_t remaining = req->content_len;
    if (expected + remaining > s_up.size) {
        xSemaphoreGive(s_lock);
        return web_send_error(req, "400 Bad Request", "chunk extends past the declared image size");
    }

    int timeouts = 0;
    while (remaining > 0) {
        if (s_up.write_err != ESP_OK) {
            fail_upload("SD card write error");
            xSemaphoreGive(s_lock);
            return web_send_error(req, "500 Internal Server Error", "SD card write error");
        }
        if (s_up.cur_idx < 0) {
            int idx;
            xQueueReceive(s_free_q, &idx, portMAX_DELAY);
            s_up.cur_idx = idx;
            s_up.cur_fill = 0;
        }
        size_t want = s_buf_bytes - s_up.cur_fill;
        if (want > remaining) {
            want = remaining;
        }
        int r = httpd_req_recv(req, (char *)s_buf[s_up.cur_idx] + s_up.cur_fill, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts < 5) {
                continue;
            }
        }
        if (r <= 0) {
            ESP_LOGW(TAG, "socket error mid-chunk (%d), keeping %llu bytes", r, (unsigned long long)s_up.dispatched);
            drain();
            discard_partial();
            xSemaphoreGive(s_lock);
            return ESP_FAIL;   // connection is gone; client resumes from upload.received in /api/status
        }
        timeouts = 0;
        mbedtls_sha256_update(&s_up.sha, s_buf[s_up.cur_idx] + s_up.cur_fill, r);
        s_up.cur_fill += r;
        remaining -= r;
        s_up.last_activity_us = esp_timer_get_time();
        if (s_up.cur_fill == s_buf_bytes) {
            dispatch_current();
        }
    }
    uint64_t received = s_up.dispatched + s_up.cur_fill;
    xSemaphoreGive(s_lock);

    cJSON *o = web_json_ok();
    cJSON_AddNumberToObject(o, "received", (double)received);
    return web_send_json(req, o);
}

static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 15];
    }
    out[2 * n] = 0;
}

static bool verify_readback(char *sha_out)
{
    uint8_t *buf = s_buf[0];
    size_t buf_sectors = s_buf_bytes / SDCARD_SECTOR_SIZE;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint64_t pos = 0;
    bool ok = true;
    s_up.verifying = true;
    while (pos < s_up.size) {
        uint64_t left = s_up.size - pos;
        size_t sectors = (size_t)((left + SDCARD_SECTOR_SIZE - 1) / SDCARD_SECTOR_SIZE);
        if (sectors > buf_sectors) {
            sectors = buf_sectors;
        }
        if (sdcard_read((uint32_t)(pos / SDCARD_SECTOR_SIZE), buf, sectors) != ESP_OK) {
            ok = false;
            break;
        }
        size_t bytes = sectors * SDCARD_SECTOR_SIZE;
        if (bytes > left) {
            bytes = (size_t)left;
        }
        mbedtls_sha256_update(&ctx, buf, bytes);
        pos += bytes;
        s_up.verify_pos = pos;
    }
    unsigned char digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    to_hex(digest, sizeof(digest), sha_out);
    s_up.verifying = false;
    return ok;
}

esp_err_t upload_finish_handler(httpd_req_t *req)
{
    char expected_sha[65] = {0};
    bool verify = settings_get()->verify_after_upload;
    cJSON *body = web_read_json_body(req);
    if (body) {
        cJSON *jsha = cJSON_GetObjectItem(body, "sha256");
        if (cJSON_IsString(jsha) && strlen(jsha->valuestring) == 64) {
            strlcpy(expected_sha, jsha->valuestring, sizeof(expected_sha));
        }
        cJSON *jv = cJSON_GetObjectItem(body, "verify");
        if (cJSON_IsBool(jv)) {
            verify = cJSON_IsTrue(jv);
        }
        cJSON_Delete(body);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_up.active) {
        xSemaphoreGive(s_lock);
        return web_send_error(req, "409 Conflict", "no upload in progress");
    }
    uint64_t total = s_up.dispatched + s_up.cur_fill;
    if (total != s_up.size) {
        xSemaphoreGive(s_lock);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "image incomplete");
        cJSON_AddNumberToObject(o, "received", (double)total);
        httpd_resp_set_status(req, "409 Conflict");
        return web_send_json(req, o);
    }
    if (s_up.cur_idx >= 0 && s_up.cur_fill > 0) {
        dispatch_current();
    } else {
        discard_partial();
    }
    drain();
    if (s_up.write_err != ESP_OK) {
        fail_upload("SD card write error");
        xSemaphoreGive(s_lock);
        return web_send_error(req, "500 Internal Server Error", "SD card write error");
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&s_up.sha, digest);
    to_hex(digest, sizeof(digest), s_up.result_sha);
    ESP_LOGI(TAG, "received %llu bytes in %.1f s, sha256 %s", (unsigned long long)total,
             (esp_timer_get_time() - s_up.started_us) / 1e6, s_up.result_sha);

    if (expected_sha[0] && strcasecmp(expected_sha, s_up.result_sha) != 0) {
        fail_upload("hash mismatch between browser and device (transfer corrupted)");
        xSemaphoreGive(s_lock);
        return web_send_error(req, "500 Internal Server Error", s_up.error);
    }

    bool verified = false;
    if (verify) {
        char readback[65];
        int64_t t0 = esp_timer_get_time();
        bool ok = verify_readback(readback);
        if (!ok) {
            fail_upload("SD card read error during verify");
            xSemaphoreGive(s_lock);
            return web_send_error(req, "500 Internal Server Error", s_up.error);
        }
        if (strcmp(readback, s_up.result_sha) != 0) {
            fail_upload("read-back verify failed: card contents do not match upload");
            xSemaphoreGive(s_lock);
            return web_send_error(req, "500 Internal Server Error", s_up.error);
        }
        verified = true;
        ESP_LOGI(TAG, "verify OK in %.1f s", (esp_timer_get_time() - t0) / 1e6);
    }

    image_meta_t meta = {0};
    strlcpy(meta.name, s_up.name, sizeof(meta.name));
    meta.size = s_up.size;
    strlcpy(meta.sha256, s_up.result_sha, sizeof(meta.sha256));
    time_t now = time(NULL);
    meta.time = now > 1600000000 ? (int64_t)now : 0;
    image_meta_save(&meta);

    end_upload(true);
    xSemaphoreGive(s_lock);

    cJSON *o = web_json_ok();
    cJSON_AddStringToObject(o, "sha256", meta.sha256);
    cJSON_AddBoolToObject(o, "verified", verified);
    return web_send_json(req, o);
}

esp_err_t upload_abort_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_up.active) {
        ESP_LOGW(TAG, "aborted by user at %llu bytes", (unsigned long long)(s_up.dispatched + s_up.cur_fill));
        strlcpy(s_up.error, "aborted", sizeof(s_up.error));
        end_upload(true);
    }
    xSemaphoreGive(s_lock);
    return web_send_ok(req);
}
