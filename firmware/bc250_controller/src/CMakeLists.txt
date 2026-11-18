# ESP-IDF component of the firmware (PlatformIO env xiao_esp32c3, platformio.ini).
# Static RAM must end below the 2.4G capture bank: checked after linking by tools/ram_guard.py.
FILE(GLOB app_sources ${CMAKE_CURRENT_LIST_DIR}/*.c ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS .
                       REQUIRES nvs_flash esp_timer driver esp_driver_gpio esp_driver_ledc
                                esp_driver_usb_serial_jtag esp_wifi esp_netif esp_event lwip
                                esp_http_server app_update bt mbedtls heap esp_hw_support
                                esp_system soc
                       PRIV_REQUIRES mdns monocypher
                       EMBED_FILES ../ui/gz/index.html.gz ../ui/gz/setup.html.gz
                       EMBED_TXTFILES ../pc_scripts/bc250-setup.sh ../pc_scripts/bc250-update.sh)

# The roaming app's header (roam_config, roam_set_config_params, ...) is a
# private include directory of esp_wifi; src/wifi_mgr.cpp uses it.
idf_component_get_property(esp_wifi_dir esp_wifi COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE ${esp_wifi_dir}/wifi_apps/roaming_app/include)
