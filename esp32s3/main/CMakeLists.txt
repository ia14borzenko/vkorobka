idf_component_register(SRCS "main.cpp"
                            "ili9486_display.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash lwip freertos esp_driver_gpio)
