idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server json lwip esp_timer esp_hw_support driver app_update esp_http_client esp_https_ota esp-tls mbedtls heap
    EMBED_TXTFILES "login.html" "admin.html" "tools.html"
)
