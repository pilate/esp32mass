#include <string.h>
#include "usb_msc.h"
#include "sdcard.h"
#include "settings.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "esp_private/usb_phy.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb";

static usb_phy_handle_t s_phy;
static TaskHandle_t s_task;
static volatile bool s_attached;
static volatile bool s_media_present;
static volatile bool s_unit_attention;
static volatile bool s_ejected;
static volatile bool s_host_mounted;
static usb_msc_stats_t s_stats;

static void tusb_device_task(void *arg)
{
    (void)arg;
    while (1) {
        tud_task();
    }
}

esp_err_t usb_msc_init(void)
{
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy), TAG, "usb phy init failed");

    tusb_rhport_init_t rh = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (!tusb_rhport_init(0, &rh)) {
        ESP_LOGE(TAG, "tusb init failed");
        return ESP_FAIL;
    }
    tud_disconnect();   // invisible to the host until explicitly attached

    if (xTaskCreatePinnedToCore(tusb_device_task, "tusb", 5120, NULL, 6, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TinyUSB ready (detached)");
    return ESP_OK;
}

void usb_msc_set_attached(bool attach)
{
    if (attach) {
        s_media_present = true;
        s_ejected = false;
        s_unit_attention = true;
        if (!s_attached) {
            s_attached = true;
            tud_connect();
            ESP_LOGI(TAG, "attached to host bus (card %s, %s)",
                     sdcard_present() ? "present" : "absent",
                     settings_get()->read_only ? "read-only" : "read-write");
        } else {
            ESP_LOGI(TAG, "media re-presented to host");
        }
    } else {
        if (s_attached) {
            tud_disconnect();
            ESP_LOGI(TAG, "detached from host bus");
        }
        s_attached = false;
        s_media_present = false;
        s_host_mounted = false;
    }
}

bool usb_msc_attached(void)
{
    return s_attached;
}

bool usb_msc_media_present(void)
{
    return s_attached && s_media_present && sdcard_present();
}

bool usb_msc_host_mounted(void)
{
    return s_attached && s_host_mounted;
}

bool usb_msc_ejected_by_host(void)
{
    return s_ejected;
}

void usb_msc_get_stats(usb_msc_stats_t *out)
{
    *out = s_stats;
}

// ---- TinyUSB device callbacks --------------------------------------------

void tud_mount_cb(void)
{
    s_host_mounted = true;
    ESP_LOGI(TAG, "host configured the device");
}

void tud_umount_cb(void)
{
    s_host_mounted = false;
    ESP_LOGI(TAG, "host released the device");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
}

// ---- MSC callbacks ---------------------------------------------------------

uint8_t tud_msc_get_maxlun_cb(void)
{
    return 1;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    const char vid[] = "ESP32";
    const char pid[] = "SD Image Drive";
    const char rev[] = "1.0";
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!s_media_present || !sdcard_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // medium not present
        return false;
    }
    if (s_unit_attention) {
        s_unit_attention = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);   // medium may have changed
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    if (s_media_present && sdcard_present()) {
        *block_count = sdcard_sector_count();
        *block_size = (uint16_t)sdcard_sector_size();
    } else {
        *block_count = 0;
        *block_size = 512;
    }
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return !settings_get()->read_only;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject) {
        if (start) {
            s_media_present = true;
            s_ejected = false;
            s_unit_attention = true;
            ESP_LOGI(TAG, "host requested media load");
        } else {
            s_media_present = false;
            s_ejected = true;
            ESP_LOGI(TAG, "host ejected the media");
        }
    }
    return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control)
{
    (void)lun;
    (void)prohibit_removal;
    (void)control;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)offset;   // always 0: EP buffer is a multiple of the sector size
    if (!s_media_present || !sdcard_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return -1;
    }
    uint32_t ss = sdcard_sector_size();
    uint32_t total = sdcard_sector_count();
    uint32_t n = bufsize / ss;
    if (lba >= total) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);   // LBA out of range
        return -1;
    }
    if (lba + n > total) {
        n = total - lba;
    }
    if (n == 0) {
        return 0;
    }
    if (sdcard_read(lba, buffer, n) != ESP_OK) {
        s_stats.read_errors++;
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // unrecovered read error
        return -1;
    }
    s_stats.bytes_read += (uint64_t)n * ss;
    s_stats.last_io_us = esp_timer_get_time();
    return (int32_t)(n * ss);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)offset;
    if (!s_media_present || !sdcard_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return -1;
    }
    if (settings_get()->read_only) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);   // write protected
        return -1;
    }
    uint32_t ss = sdcard_sector_size();
    uint32_t total = sdcard_sector_count();
    uint32_t n = bufsize / ss;
    if (lba >= total) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
        return -1;
    }
    if (lba + n > total) {
        n = total - lba;
    }
    if (n == 0) {
        return 0;
    }
    if (sdcard_write(lba, buffer, n) != ESP_OK) {
        s_stats.write_errors++;
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);   // write error
        return -1;
    }
    s_stats.bytes_written += (uint64_t)n * ss;
    s_stats.last_io_us = esp_timer_get_time();
    return (int32_t)(n * ss);
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);   // invalid command
        return -1;
    }
}
