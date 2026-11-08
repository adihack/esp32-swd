/* SPDX-License-Identifier: MIT */
/* Minimal test harness: no dependencies, one executable per suite, CTest runs them. */
#ifndef TINYTEST_H
#define TINYTEST_H
#include <stdio.h>
#include <stdint.h>
#include "esw/err.h"

static int tt_failed, tt_run;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
    tt_failed++; return; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long tt_a = (unsigned long long)(a), tt_b = (unsigned long long)(b); \
    if (tt_a != tt_b) { fprintf(stderr, "  %s:%d: %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
    tt_a, tt_b); tt_failed++; return; } } while (0)
#define CHECK_ERR(expr, want) do { esw_err_t tt_e = (expr); if (tt_e != (want)) { fprintf(stderr, \
    "  %s:%d: %s -> %s (%d), want %s\n", __FILE__, __LINE__, #expr, esw_err_str(tt_e), (int)tt_e, \
    esw_err_str(want)); tt_failed++; return; } } while (0)
#define CHECK_OK(expr) CHECK_ERR(expr, ESW_OK)

#define RUN(t) do { int tt_f0 = tt_failed; t(); tt_run++; printf("%s %s\n", tt_failed == tt_f0 ? "PASS" : "FAIL", \
    #t); } while (0)
#define TT_RESULT() (printf("%d tests, %d failed\n", tt_run, tt_failed), tt_failed ? 1 : 0)

#endif
