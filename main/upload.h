#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

esp_err_t upload_init(void);
bool upload_active(void);
void upload_check_stale(void);             // call about once a second
void upload_status_json(cJSON *obj);       // fills an object with upload state

esp_err_t upload_begin_handler(httpd_req_t *req);   // POST {name,size}
esp_err_t upload_chunk_handler(httpd_req_t *req);   // PUT ?offset=N, raw body
esp_err_t upload_finish_handler(httpd_req_t *req);  // POST {sha256?,verify?}
esp_err_t upload_abort_handler(httpd_req_t *req);   // POST
