#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "web.h"
#include "upload.h"
#include "settings.h"
#include "sdcard.h"
#include "usb_msc.h"
#include "wifi_mgr.h"
#include "weblog.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";

extern const char web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const char web_ui_html_end[] asm("_binary_web_ui_html_end");

static httpd_handle_t s_server;
static esp_timer_handle_t s_reboot_timer;

// ---- helpers ----------------------------------------------------------------

esp_err_t web_send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    if (s) {
        cJSON_free(s);
    }
    return e;
}

cJSON *web_json_ok(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return o;
}

esp_err_t web_send_ok(httpd_req_t *req)
{
    return web_send_json(req, web_json_ok());
}

esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return web_send_json(req, o);
}

cJSON *web_read_json_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > 4096) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_reboot(uint32_t ms)
{
    if (!s_reboot_timer) {
        const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
        esp_timer_create(&a, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, (uint64_t)ms * 1000);
}

// After an OTA update the bootloader starts the new app in PENDING_VERIFY state and
// rolls back to the previous one if the device resets before the app is confirmed.
// Confirm only once the web UI has actually been reached over the network, so an
// update that boots but breaks Wi-Fi or the web server is undone by a power cycle.
static void confirm_running_app(void)
{
    static bool s_confirmed;
    if (s_confirmed) {
        return;
    }
    s_confirmed = true;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "new firmware confirmed, rollback cancelled");
        } else {
            ESP_LOGE(TAG, "confirming firmware failed: %s", esp_err_to_name(err));
        }
    }
}

// ---- handlers ---------------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, web_ui_html_start, web_ui_html_end - web_ui_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    confirm_running_app();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    time_t now = time(NULL);
    cJSON_AddNumberToObject(o, "time", now > 1600000000 ? (double)now : 0);

    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddBoolToObject(w, "configured", ws.sta_configured);
    cJSON_AddBoolToObject(w, "connected", ws.sta_connected);
    cJSON_AddBoolToObject(w, "connecting", ws.sta_connecting);
    cJSON_AddStringToObject(w, "ssid", ws.sta_ssid);
    cJSON_AddStringToObject(w, "ip", ws.sta_ip);
    cJSON_AddNumberToObject(w, "rssi", ws.rssi);
    cJSON_AddBoolToObject(w, "ap_active", ws.ap_active);
    cJSON_AddStringToObject(w, "ap_ssid", ws.ap_ssid);
    cJSON_AddStringToObject(w, "ap_ip", ws.ap_ip);
    cJSON_AddNumberToObject(w, "ap_clients", ws.ap_clients);
    cJSON_AddStringToObject(w, "hostname", ws.hostname);

    char name[8];
    uint32_t freq, errors;
    sdcard_get_info(name, sizeof(name), &freq, &errors);
    cJSON *c = cJSON_AddObjectToObject(o, "card");
    cJSON_AddBoolToObject(c, "present", sdcard_present());
    cJSON_AddNumberToObject(c, "sectors", sdcard_sector_count());
    cJSON_AddNumberToObject(c, "sector_size", sdcard_sector_size());
    cJSON_AddNumberToObject(c, "bytes", (double)sdcard_capacity_bytes());
    cJSON_AddStringToObject(c, "name", name);
    cJSON_AddNumberToObject(c, "freq_khz", freq);
    cJSON_AddNumberToObject(c, "errors", errors);

    const image_meta_t *m = image_meta_get();
    cJSON *im = cJSON_AddObjectToObject(o, "image");
    cJSON_AddStringToObject(im, "name", m->name);
    cJSON_AddNumberToObject(im, "size", (double)m->size);
    cJSON_AddStringToObject(im, "sha256", m->sha256);
    cJSON_AddNumberToObject(im, "time", (double)m->time);

    usb_msc_stats_t st;
    usb_msc_get_stats(&st);
    cJSON *u = cJSON_AddObjectToObject(o, "usb");
    cJSON_AddBoolToObject(u, "attached", usb_msc_attached());
    cJSON_AddBoolToObject(u, "media", usb_msc_media_present());
    cJSON_AddBoolToObject(u, "host_mounted", usb_msc_host_mounted());
    cJSON_AddBoolToObject(u, "ejected", usb_msc_ejected_by_host());
    cJSON_AddNumberToObject(u, "bytes_read", (double)st.bytes_read);
    cJSON_AddNumberToObject(u, "bytes_written", (double)st.bytes_written);
    cJSON_AddNumberToObject(u, "read_errors", st.read_errors);
    cJSON_AddNumberToObject(u, "write_errors", st.write_errors);
    cJSON_AddNumberToObject(u, "last_io_age_s",
                            st.last_io_us ? (double)((esp_timer_get_time() - st.last_io_us) / 1000000) : -1);

    cJSON *up = cJSON_AddObjectToObject(o, "upload");
    upload_status_json(up);

    const settings_t *s = settings_get();
    cJSON *se = cJSON_AddObjectToObject(o, "settings");
    cJSON_AddBoolToObject(se, "read_only", s->read_only);
    cJSON_AddBoolToObject(se, "auto_attach", s->auto_attach);
    cJSON_AddBoolToObject(se, "verify", s->verify_after_upload);
    cJSON_AddStringToObject(se, "hostname", s->hostname);
    cJSON_AddBoolToObject(se, "ap_secured", strlen(s->ap_pass) >= 8);
    return web_send_json(req, o);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    esp_err_t err = wifi_mgr_scan(arr);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(o, "error", esp_err_to_name(err));
    }
    cJSON_AddItemToObject(o, "networks", arr);
    return web_send_json(req, o);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *b = web_read_json_body(req);
    if (!b) {
        return web_send_error(req, "400 Bad Request", "invalid JSON");
    }
    cJSON *js = cJSON_GetObjectItem(b, "ssid");
    cJSON *jp = cJSON_GetObjectItem(b, "pass");
    const char *ssid = cJSON_IsString(js) ? js->valuestring : NULL;
    const char *pass = cJSON_IsString(jp) ? jp->valuestring : "";
    esp_err_t err = wifi_mgr_set_credentials(ssid, pass);
    cJSON_Delete(b);
    if (err != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "ssid missing or too long (max 32), password max 63");
    }
    cJSON *o = web_json_ok();
    cJSON_AddStringToObject(o, "message", "connecting; watch the status panel for the new IP");
    return web_send_json(req, o);
}

static esp_err_t wifi_forget_post(httpd_req_t *req)
{
    wifi_mgr_forget();
    return web_send_ok(req);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *b = web_read_json_body(req);
    if (!b) {
        return web_send_error(req, "400 Bad Request", "invalid JSON");
    }
    settings_t s = *settings_get();
    bool ro_changed = false;
    cJSON *j;
    if (cJSON_IsBool(j = cJSON_GetObjectItem(b, "read_only"))) {
        bool v = cJSON_IsTrue(j);
        ro_changed = v != s.read_only;
        s.read_only = v;
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItem(b, "auto_attach"))) {
        s.auto_attach = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItem(b, "verify"))) {
        s.verify_after_upload = cJSON_IsTrue(j);
    }
    if (cJSON_IsString(j = cJSON_GetObjectItem(b, "hostname")) && j->valuestring[0]) {
        strlcpy(s.hostname, j->valuestring, sizeof(s.hostname));
    }
    if (cJSON_IsString(j = cJSON_GetObjectItem(b, "ap_pass"))) {
        size_t l = strlen(j->valuestring);
        if (l != 0 && l < 8) {
            cJSON_Delete(b);
            return web_send_error(req, "400 Bad Request", "AP password must be empty (open) or at least 8 characters");
        }
        strlcpy(s.ap_pass, j->valuestring, sizeof(s.ap_pass));
    }
    cJSON_Delete(b);
    esp_err_t err = settings_save(&s);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    if (ro_changed && usb_msc_attached() && !upload_active()) {
        // The host caches the write-protect flag, so re-plug to make it re-read it.
        usb_msc_set_attached(false);
        vTaskDelay(pdMS_TO_TICKS(500));
        usb_msc_set_attached(true);
    }
    return web_send_ok(req);
}

static esp_err_t usb_post(httpd_req_t *req)
{
    cJSON *b = web_read_json_body(req);
    if (!b) {
        return web_send_error(req, "400 Bad Request", "invalid JSON");
    }
    cJSON *ja = cJSON_GetObjectItem(b, "action");
    const char *action = cJSON_IsString(ja) ? ja->valuestring : "";
    esp_err_t res = ESP_OK;
    const char *msg = NULL;
    if (upload_active()) {
        msg = "busy: upload in progress";
        res = ESP_FAIL;
    } else if (strcmp(action, "attach") == 0) {
        if (!sdcard_try_mount()) {
            msg = "no SD card";
            res = ESP_FAIL;
        } else {
            usb_msc_set_attached(true);
        }
    } else if (strcmp(action, "detach") == 0) {
        usb_msc_set_attached(false);
    } else if (strcmp(action, "replug") == 0) {
        usb_msc_set_attached(false);
        vTaskDelay(pdMS_TO_TICKS(500));
        if (sdcard_try_mount()) {
            usb_msc_set_attached(true);
        }
    } else {
        msg = "unknown action";
        res = ESP_FAIL;
    }
    cJSON_Delete(b);
    if (res != ESP_OK) {
        return web_send_error(req, "409 Conflict", msg);
    }
    return web_send_ok(req);
}

static esp_err_t download_get(httpd_req_t *req)
{
    if (upload_active()) {
        return web_send_error(req, "409 Conflict", "upload in progress");
    }
    if (usb_msc_attached()) {
        return web_send_error(req, "409 Conflict", "detach from USB first so the host cannot change the data mid-download");
    }
    if (!sdcard_try_mount()) {
        return web_send_error(req, "409 Conflict", "no SD card");
    }
    const image_meta_t *m = image_meta_get();
    uint64_t total = m->size;
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "full", val, sizeof(val)) == ESP_OK && val[0] == '1') {
        total = sdcard_capacity_bytes();
    }
    if (total == 0) {
        return web_send_error(req, "404 Not Found", "no image recorded; use ?full=1 to dump the whole card");
    }
    const size_t buf_sectors = 64;
    uint8_t *buf = heap_caps_malloc(buf_sectors * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        return web_send_error(req, "500 Internal Server Error", "out of memory");
    }
    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", m->name[0] ? m->name : "sdcard.img");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint64_t pos = 0;
    esp_err_t err = ESP_OK;
    while (pos < total) {
        uint64_t left = total - pos;
        size_t sectors = (size_t)((left + 511) / 512);
        if (sectors > buf_sectors) {
            sectors = buf_sectors;
        }
        if (sdcard_read((uint32_t)(pos / 512), buf, sectors) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        size_t bytes = sectors * 512;
        if (bytes > left) {
            bytes = (size_t)left;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, bytes) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        pos += bytes;
    }
    heap_caps_free(buf);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    } else {
        httpd_resp_sendstr_chunk(req, NULL);
    }
    return err;
}

#define WIPE_HEAD_SECTORS 4096   // first 2 MB: MBR, primary GPT, boot sectors, first FAT copies
#define WIPE_TAIL_SECTORS 2048   // last 1 MB: backup GPT header and entries (last 33 sectors)

static esp_err_t zero_sectors(const uint8_t *zeros, size_t buf_sectors, uint32_t lba, uint32_t end)
{
    esp_err_t err = ESP_OK;
    while (lba < end && err == ESP_OK) {
        uint32_t n = end - lba < buf_sectors ? end - lba : buf_sectors;
        err = sdcard_write(lba, zeros, n);
        lba += n;
    }
    return err;
}

static esp_err_t wipe_post(httpd_req_t *req)
{
    if (upload_active()) {
        return web_send_error(req, "409 Conflict", "upload in progress");
    }
    if (!sdcard_try_mount()) {
        return web_send_error(req, "409 Conflict", "no SD card");
    }
    // Allocate before detaching so running out of memory leaves the USB state alone.
    const size_t buf_sectors = 64;
    uint8_t *zeros = heap_caps_calloc(1, buf_sectors * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!zeros) {
        return web_send_error(req, "500 Internal Server Error", "out of memory");
    }
    bool was_attached = usb_msc_attached();
    usb_msc_set_attached(false);

    // Zero both ends of the card. Clearing only the start leaves the backup GPT at
    // the end, which partitioning tools then offer to "restore".
    uint32_t total = sdcard_sector_count();
    uint32_t head_end = total < WIPE_HEAD_SECTORS ? total : WIPE_HEAD_SECTORS;
    uint32_t tail_start = total > WIPE_TAIL_SECTORS ? total - WIPE_TAIL_SECTORS : 0;
    if (tail_start < head_end) {
        tail_start = head_end;   // tiny card: the two ranges overlap
    }
    esp_err_t err = zero_sectors(zeros, buf_sectors, 0, head_end);
    if (err == ESP_OK) {
        err = zero_sectors(zeros, buf_sectors, tail_start, total);
    }
    heap_caps_free(zeros);
    image_meta_clear();
    if (was_attached) {
        usb_msc_set_attached(true);
    }
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "write failed");
    }
    ESP_LOGW(TAG, "card wiped (first 2 MB and last 1 MB)");
    return web_send_ok(req);
}

static esp_err_t log_get(httpd_req_t *req)
{
    char *buf = malloc(8192);
    if (!buf) {
        return web_send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t n = weblog_copy(buf, 8192);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, buf, n);
    free(buf);
    return e;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (upload_active()) {
        return web_send_error(req, "409 Conflict", "upload in progress");
    }
    confirm_running_app();   // esp_ota_begin() refuses while the running app is unconfirmed
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return web_send_error(req, "500 Internal Server Error", "no OTA partition");
    }
    if (req->content_len < 4096 || req->content_len > part->size) {
        return web_send_error(req, "400 Bad Request", "firmware size out of range");
    }
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(h);
        return web_send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t remaining = req->content_len;
    bool first = true;
    int timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;   // a stalled client gets ~100 s, then the server is freed again
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (first) {
            first = false;
            if ((uint8_t)buf[0] != 0xE9) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) {
            break;
        }
        timeouts = 0;
        remaining -= r;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        return web_send_error(req, "400 Bad Request", err == ESP_ERR_INVALID_ARG ? "not an ESP32 app image" : "upload interrupted");
    }
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "firmware image failed validation");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "firmware updated into %s, rebooting", part->label);
    schedule_reboot(1000);
    cJSON *o = web_json_ok();
    cJSON_AddStringToObject(o, "message", "firmware written, rebooting");
    return web_send_json(req, o);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    schedule_reboot(500);
    return web_send_ok(req);
}

// State-changing requests must carry this header. A browser only sends a custom
// header cross-origin after a CORS preflight, which this server never approves,
// so this stops other websites from driving the API (wipe, OTA, ...) through
// the user's browser.
#define CSRF_HEADER "X-Requested-With"

typedef esp_err_t (*handler_fn_t)(httpd_req_t *req);

static esp_err_t csrf_guard(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, CSRF_HEADER) == 0) {
        ESP_LOGW(TAG, "rejected %s without " CSRF_HEADER " header", req->uri);
        return web_send_error(req, "403 Forbidden", "missing " CSRF_HEADER " header");
    }
    return ((handler_fn_t)req->user_ctx)(req);
}

// Captive portal: anything we do not know redirects to the UI.
static esp_err_t redirect_get(httpd_req_t *req)
{
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    char loc[48];
    snprintf(loc, sizeof(loc), "http://%s/", ws.ap_active && ws.ap_ip[0] ? ws.ap_ip : (ws.sta_ip[0] ? ws.sta_ip : "192.168.4.1"));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "redirecting to setup page");
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 1;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/",                   .method = HTTP_GET,  .handler = root_get },
        { .uri = "/index.html",         .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status",         .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scan",           .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/wifi",           .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/wifi/forget",    .method = HTTP_POST, .handler = wifi_forget_post },
        { .uri = "/api/settings",       .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/usb",            .method = HTTP_POST, .handler = usb_post },
        { .uri = "/api/upload/begin",   .method = HTTP_POST, .handler = upload_begin_handler },
        { .uri = "/api/upload/chunk",   .method = HTTP_PUT,  .handler = upload_chunk_handler },
        { .uri = "/api/upload/finish",  .method = HTTP_POST, .handler = upload_finish_handler },
        { .uri = "/api/upload/abort",   .method = HTTP_POST, .handler = upload_abort_handler },
        { .uri = "/api/download",       .method = HTTP_GET,  .handler = download_get },
        { .uri = "/api/wipe",           .method = HTTP_POST, .handler = wipe_post },
        { .uri = "/api/log",            .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/ota",            .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/reboot",         .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/*",                  .method = HTTP_GET,  .handler = redirect_get },   // keep last
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t r = routes[i];
        if (r.method != HTTP_GET) {
            r.user_ctx = (void *)r.handler;
            r.handler = csrf_guard;
        }
        httpd_register_uri_handler(s_server, &r);
    }
    ESP_LOGI(TAG, "web UI on port 80");
    return ESP_OK;
}
