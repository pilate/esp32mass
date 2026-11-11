#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

esp_err_t web_start(void);

// Helpers shared with upload.c
esp_err_t web_send_json(httpd_req_t *req, cJSON *obj);               // takes ownership of obj
esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *msg);
cJSON *web_read_json_body(httpd_req_t *req);                          // NULL on error
