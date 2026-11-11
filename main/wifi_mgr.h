#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct {
    bool sta_configured;
    bool sta_connected;
    bool sta_connecting;
    char sta_ssid[33];
    char sta_ip[16];
    bool ap_active;
    char ap_ssid[33];
    char ap_ip[16];
    int ap_clients;
    char hostname[32];
} wifi_mgr_status_t;

esp_err_t wifi_mgr_start(void);
void wifi_mgr_get_status(wifi_mgr_status_t *out);   // cheap: flags and cached strings only
int wifi_mgr_get_rssi(void);                        // queries the driver; 0 if not connected
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass);
esp_err_t wifi_mgr_forget(void);
esp_err_t wifi_mgr_scan(cJSON *out_array);   // blocking, a few seconds
