#include <string.h>
#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
#define NS "esp32mass"

static settings_t s_cfg;
static image_meta_t s_img;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        // keep default already in dst
    }
}

static void load_bool(nvs_handle_t h, const char *key, bool *dst)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *dst = v != 0;
    }
}

static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_img, 0, sizeof(s_img));
    strlcpy(s_cfg.hostname, "esp32mass", sizeof(s_cfg.hostname));
    strlcpy(s_cfg.ap_pass, "esp32mass", sizeof(s_cfg.ap_pass));
    s_cfg.read_only = false;
    s_cfg.auto_attach = true;
    s_cfg.verify_after_upload = true;
}

esp_err_t settings_init(void)
{
    set_defaults();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings (%s), using defaults", esp_err_to_name(err));
        return ESP_OK;
    }
    load_str(h, "ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_str(h, "pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    load_str(h, "host", s_cfg.hostname, sizeof(s_cfg.hostname));
    load_str(h, "ap_pass", s_cfg.ap_pass, sizeof(s_cfg.ap_pass));
    load_bool(h, "ro", &s_cfg.read_only);
    load_bool(h, "autoatt", &s_cfg.auto_attach);
    load_bool(h, "verify", &s_cfg.verify_after_upload);

    load_str(h, "img_name", s_img.name, sizeof(s_img.name));
    nvs_get_u64(h, "img_size", &s_img.size);
    load_str(h, "img_sha", s_img.sha256, sizeof(s_img.sha256));
    nvs_get_i64(h, "img_time", &s_img.time);
    nvs_close(h);
    if (s_cfg.hostname[0] == 0) {
        strlcpy(s_cfg.hostname, "esp32mass", sizeof(s_cfg.hostname));
    }
    ESP_LOGI(TAG, "loaded: ssid='%s' host='%s' ro=%d auto=%d image='%s' (%llu bytes)",
             s_cfg.wifi_ssid, s_cfg.hostname, s_cfg.read_only, s_cfg.auto_attach,
             s_img.name, (unsigned long long)s_img.size);
    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

esp_err_t settings_save(const settings_t *s)
{
    if (s != &s_cfg) {
        s_cfg = *s;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", s_cfg.wifi_ssid);
    nvs_set_str(h, "pass", s_cfg.wifi_pass);
    nvs_set_str(h, "host", s_cfg.hostname);
    nvs_set_str(h, "ap_pass", s_cfg.ap_pass);
    nvs_set_u8(h, "ro", s_cfg.read_only);
    nvs_set_u8(h, "autoatt", s_cfg.auto_attach);
    nvs_set_u8(h, "verify", s_cfg.verify_after_upload);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    strlcpy(s_cfg.wifi_ssid, ssid ? ssid : "", sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_pass, pass ? pass : "", sizeof(s_cfg.wifi_pass));
    return settings_save(&s_cfg);
}

esp_err_t settings_clear_wifi(void)
{
    return settings_set_wifi("", "");
}

bool settings_has_wifi(void)
{
    return s_cfg.wifi_ssid[0] != 0;
}

const image_meta_t *image_meta_get(void)
{
    return &s_img;
}

esp_err_t image_meta_save(const image_meta_t *m)
{
    if (m != &s_img) {
        s_img = *m;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "img_name", s_img.name);
    nvs_set_u64(h, "img_size", s_img.size);
    nvs_set_str(h, "img_sha", s_img.sha256);
    nvs_set_i64(h, "img_time", s_img.time);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t image_meta_clear(void)
{
    memset(&s_img, 0, sizeof(s_img));
    return image_meta_save(&s_img);
}
