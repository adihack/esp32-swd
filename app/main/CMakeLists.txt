idf_component_register(SRCS "main.c"
                       REQUIRES esw_lib esw_port esw_board esp_driver_usb_serial_jtag esp_timer esp_system freertos app_update
                                esp_partition)
# Panic safety: board_isolate_now() runs first thing in the panic handler (see __wrap_esp_panic_handler).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
# Test hooks (TEST_PANIC command) in this test firmware.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ESW_TEST_HOOKS=1)
