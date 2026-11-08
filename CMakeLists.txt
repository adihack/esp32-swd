# esp-swd: host build of the portable library and its unit tests.
# The ESP-IDF application (app/, milestone M1) will consume lib/ as a component.
cmake_minimum_required(VERSION 3.16)
project(esp_swd C)

set(ESW_TARGETS "nrf52" CACHE STRING "Target modules to build (semicolon-separated, see lib/targets/)")
option(ESW_BUILD_TESTS "Build host unit tests" ON)
option(ESW_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(ESW_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(lib)
if(ESW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
