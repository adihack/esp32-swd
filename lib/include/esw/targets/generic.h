/* SPDX-License-Identifier: MIT */
/* esp-swd target module: generic Cortex-M fallback. Registered last, it takes any target no chip module claimed:
 * memory access through AP<core> (default AP0), halt, resume and reset, but no flash regions (nothing to erase or
 * program). Useful to identify a new part and to bring up its module. */
#ifndef ESW_TARGETS_GENERIC_H
#define ESW_TARGETS_GENERIC_H
#include "esw/target.h"

extern const esw_target_driver_t esw_target_generic;

#endif
