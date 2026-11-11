#include <string.h>
#include <stdio.h>
#include "wifi_mgr.h"
#include "settings.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mdns.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi";

#define AP_FALLBACK_S       20   // STA down this long -> bring up the access point
#define AP_LINGER_S         120  // STA up and AP idle this long -> drop the access point
#define RECONNECT_DELAY_S   3
#define CONNECT_WATCHDOG_S  30

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_lock;
static dns_server_handle_t s_dns;

static volatile bool s_sta_connected;
static volatile bool s_sta_connecting;
static volatile bool s_ap_active;
static volatile bool s_scanning;
static bool s_sntp_started;
static int64_t s_boot_us;
static int64_t s_last_connected_us;   // last time STA had an IP (or creds changed)
static int64_t s_next_connect_us;     // when to try esp_wifi_connect next
static int64_t s_connect_started_us;
static int64_t s_ap_up_us;
static int s_ap_clients;
static char s_ap_ssid[33];
static char s_sta_ip[16];

static void apply_sta_config(void)
{
    const settings_t *s = settings_get();
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, s->wifi_ssid, strnlen(s->wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, s->wifi_pass, strnlen(s->wifi_pass, sizeof(wc.sta.password)));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = s->wifi_pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config: %s", esp_err_to_name(err));
    }
}

static void set_ap_enabled(bool on)
{
    if (on == s_ap_active) {
        return;
    }
    if (on) {
        const settings_t *s = settings_get();
        wifi_config_t ac = {0};
        size_t l = strlen(s_ap_ssid);
        memcpy(ac.ap.ssid, s_ap_ssid, l);
        ac.ap.ssid_len = l;
        ac.ap.channel = 1;
        ac.ap.max_connection = 4;
        if (strlen(s->ap_pass) >= 8) {
            memcpy(ac.ap.password, s->ap_pass, strnlen(s->ap_pass, sizeof(ac.ap.password)));
            ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ac.ap.authmode = WIFI_AUTH_OPEN;
        }
        ac.ap.pmf_cfg.required = false;
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_AP, &ac);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
            return;
        }
        dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&dc);
        s_ap_active = true;
        s_ap_up_us = esp_timer_get_time();
        s_ap_clients = 0;
        ESP_LOGI(TAG, "AP '%s' up (%s), portal at 192.168.4.1", s_ap_ssid,
                 ac.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    } else {
        if (s_dns) {
            stop_dns_server(s_dns);
            s_dns = NULL;
        }
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_active = false;
        ESP_LOGI(TAG, "AP down");
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            s_next_connect_us = 0;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "associated, waiting for IP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = data;
            if (s_sta_connected) {
                ESP_LOGW(TAG, "disconnected from '%s' (reason %d)", settings_get()->wifi_ssid, e->reason);
            } else if (s_sta_connecting) {
                ESP_LOGW(TAG, "connect to '%s' failed (reason %d)", settings_get()->wifi_ssid, e->reason);
            }
            s_sta_connected = false;
            s_sta_connecting = false;
            s_sta_ip[0] = 0;
            s_next_connect_us = esp_timer_get_time() + RECONNECT_DELAY_S * 1000000LL;
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            ESP_LOGI(TAG, "AP client joined (%d)", s_ap_clients);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) {
                s_ap_clients--;
            }
            ESP_LOGI(TAG, "AP client left (%d)", s_ap_clients);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_connected = true;
        s_sta_connecting = false;
        s_last_connected_us = esp_timer_get_time();
        ESP_LOGI(TAG, "connected to '%s', IP %s", settings_get()->wifi_ssid, s_sta_ip);
        if (!s_sntp_started) {
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&c) == ESP_OK) {
                s_sntp_started = true;
            }
        }
    }
}

static void mgr_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool have = settings_has_wifi();

        if (have && !s_sta_connected && !s_sta_connecting && !s_scanning && now >= s_next_connect_us) {
            apply_sta_config();
            esp_err_t e = esp_wifi_connect();
            if (e == ESP_OK) {
                s_sta_connecting = true;
                s_connect_started_us = now;
                ESP_LOGI(TAG, "connecting to '%s'", settings_get()->wifi_ssid);
            } else {
                ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
            }
            s_next_connect_us = now + RECONNECT_DELAY_S * 1000000LL;
        }
        if (s_sta_connecting && now - s_connect_started_us > CONNECT_WATCHDOG_S * 1000000LL) {
            ESP_LOGW(TAG, "connect attempt timed out");
            s_sta_connecting = false;
            esp_wifi_disconnect();
        }

        bool want_ap;
        if (!have) {
            want_ap = true;
        } else if (s_sta_connected) {
            bool recently = (now - s_last_connected_us < AP_LINGER_S * 1000000LL) ||
                            (now - s_ap_up_us < AP_LINGER_S * 1000000LL);
            want_ap = s_ap_active && (s_ap_clients > 0 || recently);
        } else {
            int64_t ref = s_last_connected_us > s_boot_us ? s_last_connected_us : s_boot_us;
            want_ap = s_ap_active || (now - ref > AP_FALLBACK_S * 1000000LL);
        }
        if (want_ap != s_ap_active) {
            set_ap_enabled(want_ap);
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t wifi_mgr_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_boot_us = esp_timer_get_time();
    s_next_connect_us = 0;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL),
                        TAG, "register wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL),
                        TAG, "register ip events");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "esp32mass-%02X%02X", mac[4], mac[5]);

    const char *host = settings_get()->hostname;
    esp_netif_set_hostname(s_sta_netif, host);
    esp_netif_set_hostname(s_ap_netif, host);

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    if (settings_has_wifi()) {
        apply_sta_config();
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set("ESP32 SD Image Drive");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: http://%s.local/", host);
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    }

    if (xTaskCreate(mgr_task, "wifi_mgr", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    memset(out, 0, sizeof(*out));
    const settings_t *s = settings_get();
    out->sta_configured = settings_has_wifi();
    out->sta_connected = s_sta_connected;
    out->sta_connecting = s_sta_connecting;
    strlcpy(out->sta_ssid, s->wifi_ssid, sizeof(out->sta_ssid));
    strlcpy(out->sta_ip, s_sta_ip, sizeof(out->sta_ip));
    strlcpy(out->hostname, s->hostname, sizeof(out->hostname));
    out->ap_active = s_ap_active;
    out->ap_clients = s_ap_clients;
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    if (s_ap_active && s_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&ip.ip));
        }
    }
}

int wifi_mgr_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    // Update the stored credentials under s_lock: mgr_task reads them (apply_sta_config)
    // while holding it, and must not see a half-copied SSID or password.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = settings_set_wifi(ssid, pass);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    if (s_sta_connected || s_sta_connecting) {
        esp_wifi_disconnect();
    }
    s_sta_connected = false;
    s_sta_connecting = false;
    s_sta_ip[0] = 0;
    s_last_connected_us = esp_timer_get_time();   // restart the AP fallback timer
    s_next_connect_us = 0;
    apply_sta_config();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "credentials updated for '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_forget(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_clear_wifi();
    if (s_sta_connected || s_sta_connecting) {
        esp_wifi_disconnect();
    }
    s_sta_connected = false;
    s_sta_connecting = false;
    s_sta_ip[0] = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "credentials cleared");
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(cJSON *out_array)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scanning = true;
    if (s_sta_connecting) {
        esp_wifi_disconnect();
        s_sta_connecting = false;
    }
    xSemaphoreGive(s_lock);

    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 24) {
            n = 24;
        }
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        if (recs) {
            esp_wifi_scan_get_ap_records(&n, recs);
            for (uint16_t i = 0; i < n; i++) {
                if (recs[i].ssid[0] == 0) {
                    continue;
                }
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
                cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
                cJSON_AddNumberToObject(o, "chan", recs[i].primary);
                cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(out_array, o);
            }
            free(recs);
        } else {
            esp_wifi_clear_ap_list();
        }
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    s_scanning = false;
    s_next_connect_us = 0;
    return err;
}
