#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

typedef struct {
    bool active;
    bool verifying;         // upload complete, background read-back verify running
    uint64_t verify_pos;    // bytes verified so far
    uint64_t size;          // declared image size in bytes
    uint64_t received;      // bytes accepted so far
    double rate_bps;        // average receive rate, 0 when idle or just started
} upload_progress_t;

esp_err_t upload_init(void);
bool upload_active(void);
void upload_get_progress(upload_progress_t *out);
void upload_check_stale(void);             // call about once a second
void upload_status_json(cJSON *obj);       // fills an object with upload state

esp_err_t upload_begin_handler(httpd_req_t *req);   // POST {name,size}
esp_err_t upload_chunk_handler(httpd_req_t *req);   // PUT ?offset=N, raw body
esp_err_t upload_finish_handler(httpd_req_t *req);  // POST {sha256?,verify?}; 202 while verifying
esp_err_t upload_abort_handler(httpd_req_t *req);   // POST
