idf_component_register(
    SRCS tusb_composite_main.c usb_descriptors.c wifi_sta.c wifi_dap_tcp.c usbip_server.c web_server.c usb_msc.c logfs.c
    INCLUDE_DIRS .
    REQUIRES cmsis_dap esp_wifi esp_event esp_netif nvs_flash esp_http_server mdns esp_psram fatfs wear_levelling app_update
    EMBED_FILES "web/xterm.css.gz" "web/xterm.js.gz" "web/xterm-addon-fit.js.gz"
)
