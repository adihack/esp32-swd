/* SPDX-License-Identifier: MIT */
/* Target-module registry. Each module is enabled at build time (CMake option ESW_TARGETS -> ESW_TARGET_<NAME>).
 * Adding a chip family: lib/targets/<name>/, a header in include/esw/targets/, and one line here. */
#include "esw/target.h"

#ifdef ESW_TARGET_NRF52
#include "esw/targets/nrf52.h"
#endif

const esw_target_driver_t *const esw_target_drivers[] = {
#ifdef ESW_TARGET_NRF52
    &esw_target_nrf52,
#endif
    0,
};
