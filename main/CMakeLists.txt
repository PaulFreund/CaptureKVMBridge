set(app_priv_requires
    esp_driver_gpio
    esp_lcd_touch
    esp_lcd_touch_gt911
    esp_lvgl_port
    esp32_p4_wifi6_touch_lcd_4b
    esp_tinyusb
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    driver
)

if(CONFIG_APP_ENABLE_USB_AUDIO)
    list(APPEND app_priv_requires usb_device_uac)
endif()

set(UAC_MANAGED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__usb_device_uac")
set(UAC_MANAGED_TUSB_DIR "${UAC_MANAGED_DIR}/tusb_uac")

idf_component_register(
    SRCS
        "hid_forwarder_main.c"
        "app_state.c"
        "usb_hs_device.c"
        "protocol_tlv.c"
        "display_ui.c"
        "network_transport.c"
        "serial_transport.c"
        "uart_transport.c"
        "tlv_stream.c"
    INCLUDE_DIRS
        "."
    PRIV_INCLUDE_DIRS
        "${UAC_MANAGED_DIR}"
        "${UAC_MANAGED_TUSB_DIR}"
    PRIV_REQUIRES ${app_priv_requires}
)

idf_component_get_property(_uac_dir espressif__usb_device_uac COMPONENT_DIR)
idf_component_get_property(_esp_tusb_dir espressif__esp_tinyusb COMPONENT_DIR)

idf_component_get_property(_tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${_tusb_lib} BEFORE PUBLIC "tusb_include")
target_include_directories(${_tusb_lib} BEFORE PRIVATE "tusb_include")
target_include_directories(${_tusb_lib} PRIVATE "${_uac_dir}/tusb_uac")

idf_component_get_property(_esp_tusb_lib espressif__esp_tinyusb COMPONENT_LIB)
target_include_directories(${_esp_tusb_lib} BEFORE PUBLIC "tusb_include")
target_include_directories(${_esp_tusb_lib} BEFORE PRIVATE "tusb_include")
target_include_directories(${_esp_tusb_lib} PRIVATE "${_uac_dir}/tusb_uac")

idf_component_get_property(_uac_lib espressif__usb_device_uac COMPONENT_LIB)
target_include_directories(${_uac_lib} BEFORE PUBLIC "tusb_include")
target_include_directories(${_uac_lib} BEFORE PRIVATE "tusb_include")
target_include_directories(${_uac_lib} PRIVATE "${_esp_tusb_dir}/include")
target_include_directories(${_uac_lib} PRIVATE "${_uac_dir}/tusb_uac")
