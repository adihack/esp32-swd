set(P "${CMAKE_CURRENT_LIST_DIR}/../../../ports/esp32s3")
idf_component_register(SRCS "${P}/esw_port.c" "${P}/esw_dgpio.c" "${P}/esw_uart.c"
                       INCLUDE_DIRS "${P}/include" PRIV_INCLUDE_DIRS "${P}"
                       REQUIRES esw_lib esp_driver_gpio esp_hal_gpio esp_driver_uart esp_hal_uart esp_driver_dma esp_hal_dma esp_rom esp_hw_support esp_adc
                                esp_timer freertos)
