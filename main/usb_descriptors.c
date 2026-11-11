#include <string.h>
#include <stdio.h>
#include "tusb.h"
#include "esp_mac.h"

#define USB_VID 0x303A   // Espressif
#define USB_PID 0x4002   // TinyUSB MSC-only PID used by esp_tinyusb

static const tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_desc_device;
}

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_desc_configuration[] = {
    // Bus powered, up to 500 mA (Wi-Fi bursts)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

static char s_serial[13];

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},   // 0: English (US)
    "esp32mass",                  // 1: manufacturer
    "SD Image Drive",             // 2: product
    s_serial,                     // 3: serial
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[33];
    size_t chr_count;

    if (index == 0) {
        memcpy(&desc[1], s_string_desc[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(s_string_desc) / sizeof(s_string_desc[0])) {
            return NULL;
        }
        if (index == 3 && s_serial[0] == 0) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        const char *str = s_string_desc[index];
        chr_count = strlen(str);
        if (chr_count > 32) {
            chr_count = 32;
        }
        for (size_t i = 0; i < chr_count; i++) {
            desc[1 + i] = str[i];
        }
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc;
}
