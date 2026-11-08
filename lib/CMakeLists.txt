# Portable esp-swd library: C11, no RTOS, no vendor headers.
set(ESW_CORE_SRC
  core/err.c
  core/swd/swd_bitbang.c
  core/swd/adiv5.c
  core/cpu/cortexm.c
  core/loader/loader.c
  core/loader/crc32_loader.c
  core/target/target.c
  channel/channel.c
  proto/proto.c
  targets/registry.c
)

set(ESW_TARGET_SRC "")
set(ESW_TARGET_DEFS "")
foreach(t IN LISTS ESW_TARGETS)
  string(TOUPPER "${t}" T)
  list(APPEND ESW_TARGET_SRC "targets/${t}/${t}.c")
  list(APPEND ESW_TARGET_DEFS "ESW_TARGET_${T}")
endforeach()

add_library(esw STATIC ${ESW_CORE_SRC} ${ESW_TARGET_SRC})
target_include_directories(esw PUBLIC include)
target_compile_definitions(esw PUBLIC ${ESW_TARGET_DEFS})
target_compile_features(esw PUBLIC c_std_11)
set_target_properties(esw PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(esw PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wconversion -Werror)
endif()
