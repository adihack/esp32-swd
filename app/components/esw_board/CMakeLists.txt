set(B "${CMAKE_CURRENT_LIST_DIR}/../../../boards/r5")
idf_component_register(SRCS "${B}/board_r5.c" INCLUDE_DIRS "${B}/include"
                       REQUIRES esw_lib esw_port esp_driver_gpio freertos)
