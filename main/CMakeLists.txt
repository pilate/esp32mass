idf_component_register(
    SRCS
        "main.c"
        "settings.c"
        "sdcard.c"
        "usb_msc.c"
        "usb_descriptors.c"
        "wifi_mgr.c"
        "web.c"
        "upload.c"
        "status_led.c"
        "display.c"
        "weblog.c"
    INCLUDE_DIRS "." "tusb"
    EMBED_TXTFILES "web_ui.html"
    PRIV_REQUIRES
        esp_driver_sdmmc sdmmc usb esp_wifi esp_netif esp_event nvs_flash
        esp_http_server json mbedtls app_update esp_app_format esp_timer
        esp_driver_gpio esp_driver_spi esp_lcd lwip dns_server
)

# Standalone TinyUSB needs to be told where our tusb_config.h lives.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tusb")

# usb_descriptors.c is only referenced from inside the TinyUSB library, so make
# sure the linker keeps its callbacks.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u tud_descriptor_device_cb"
    "-u tud_descriptor_configuration_cb"
    "-u tud_descriptor_string_cb")
