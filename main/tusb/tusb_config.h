// TinyUSB configuration for esp32mass (ESP32-S3, full-speed device, MSC only)
#pragma once

#include "sdkconfig.h"
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/

// DMA mode for the DWC2 core, matching what esp_tinyusb uses on the S3.
#define CFG_TUD_DWC2_DMA_ENABLE     1
#define CFG_TUD_MEM_DCACHE_ENABLE   0
#define CFG_TUSB_MEM_SECTION        DRAM_DMA_ALIGNED_ATTR
#define CFG_TUSB_MEM_ALIGN          TU_ATTR_ALIGNED(4)

#define CFG_TUSB_DEBUG              0
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 1
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

// Bytes transferred per read10/write10 callback. Multiple of the 512 byte sector.
#define CFG_TUD_MSC_EP_BUFSIZE      8192

#ifdef __cplusplus
}
#endif
