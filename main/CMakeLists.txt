idf_component_register(SRCS "esp32-ota.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES 
                        esp_wifi
                        esp_event
                        nvs_flash
                        esp_http_client
                        esp-tls)
