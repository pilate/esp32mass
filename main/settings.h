#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char hostname[32];
    char ap_pass[65];        // empty = open access point
    bool read_only;          // present the drive write-protected to the host
    bool auto_attach;        // attach to USB automatically when a card is present
    bool verify_after_upload;// read the image back and hash it after upload
} settings_t;

typedef struct {
    char name[96];
    uint64_t size;           // bytes, 0 = no image recorded
    char sha256[65];         // hex, empty if unknown
    int64_t time;            // unix seconds, 0 if clock was not set
} image_meta_t;

esp_err_t settings_init(void);
const settings_t *settings_get(void);
esp_err_t settings_save(const settings_t *s);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);
esp_err_t settings_clear_wifi(void);
bool settings_has_wifi(void);

const image_meta_t *image_meta_get(void);
esp_err_t image_meta_save(const image_meta_t *m);
esp_err_t image_meta_clear(void);
