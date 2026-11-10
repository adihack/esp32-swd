/* SPDX-License-Identifier: MIT */
/* Target-module registry. Each module is enabled at build time (CMake option ESW_TARGETS -> ESW_TARGET_<NAME>).
 * Adding a chip family: lib/targets/<name>/, a header in include/esw/targets/, and one line here. */
#include "esw/target.h"

#ifdef ESW_TARGET_NRF52
#include "esw/targets/nrf52.h"
#endif
#ifdef ESW_TARGET_NRF53
#include "esw/targets/nrf53.h"
#endif
#ifdef ESW_TARGET_STM32U5
#include "esw/targets/stm32u5.h"
#endif
#ifdef ESW_TARGET_GENERIC
#include "esw/targets/generic.h"
#endif

const esw_target_driver_t *const esw_target_drivers[] = {
#ifdef ESW_TARGET_NRF52
    &esw_target_nrf52,
#endif
#ifdef ESW_TARGET_NRF53
    &esw_target_nrf53,
#endif
#ifdef ESW_TARGET_STM32U5
    &esw_target_stm32u5,
#endif
#ifdef ESW_TARGET_GENERIC
    &esw_target_generic,   /* last: takes any Cortex-M no chip module claimed */
#endif
    0,
};
