idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_driver_i2s esp_driver_sdm esp_driver_gpio esp_timer)
