# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.


idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_https_ota
        nvs_flash
        json
        mbedtls
        app_update
)