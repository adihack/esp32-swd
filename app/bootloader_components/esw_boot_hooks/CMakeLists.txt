# 2nd-stage bootloader hook: isolation switches open within microseconds of any reset (a panic or CPU reset keeps
# the GPIO output latches, so without this the enables would stay high until app_main).
idf_component_register(SRCS "hooks.c"
                       INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../boards/r5/include")
