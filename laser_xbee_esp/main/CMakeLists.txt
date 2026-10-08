idf_component_register(SRCS "main.c" "xbee_frame.c" "radio.c"
                       INCLUDE_DIRS "."
                       REQUIRES ieee802154 esp_driver_gpio nvs_flash)
