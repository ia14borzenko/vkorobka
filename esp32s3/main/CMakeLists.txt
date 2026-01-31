idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES .common
                    REQUIRES esp_wifi nvs_flash lwip freertos)
