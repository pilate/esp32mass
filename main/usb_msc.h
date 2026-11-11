#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint32_t read_errors;
    uint32_t write_errors;
    int64_t last_io_us;      // esp_timer time of last host IO, 0 if none
} usb_msc_stats_t;

esp_err_t usb_msc_init(void);               // starts with the USB pull-up disconnected
void usb_msc_set_attached(bool attach);     // connect/disconnect from the host bus
bool usb_msc_attached(void);                // we are presenting ourselves on the bus
bool usb_msc_media_present(void);           // host is told a medium is present
bool usb_msc_host_mounted(void);            // host configured the device
bool usb_msc_ejected_by_host(void);
void usb_msc_get_stats(usb_msc_stats_t *out);
