# The portable library (../../../lib) as an ESP-IDF component. Target modules: add sources + ESW_TARGET_<NAME>.
set(L "${CMAKE_CURRENT_LIST_DIR}/../../../lib")
idf_component_register(
  SRCS "${L}/core/err.c" "${L}/core/swd/swd_bitbang.c" "${L}/core/swd/adiv5.c" "${L}/core/cpu/cortexm.c"
       "${L}/core/loader/loader.c" "${L}/core/loader/crc32_loader.c" "${L}/core/target/target.c" "${L}/channel/channel.c" "${L}/proto/proto.c"
       "${L}/targets/registry.c" "${L}/targets/nrf52/nrf52.c" "${L}/targets/nrf53/nrf53.c"
       "${L}/targets/stm32u5/stm32u5.c" "${L}/targets/generic/generic.c"
  INCLUDE_DIRS "${L}/include")
target_compile_definitions(${COMPONENT_LIB} PUBLIC ESW_TARGET_NRF52 ESW_TARGET_NRF53 ESW_TARGET_STM32U5
                                                         ESW_TARGET_GENERIC)
