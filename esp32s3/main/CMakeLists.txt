idf_component_register(SRCS "main.cpp" "wifi.cpp" "tcp.cpp" "message_bridge.cpp" "uart_bridge.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES .common
                    REQUIRES esp_wifi nvs_flash lwip freertos driver)
