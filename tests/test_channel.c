/* SPDX-License-Identifier: MIT */
/* Isolation interlock: order of switch operations, VTref gating, holders, host loss. */
#include "tinytest.h"
#include <string.h>
#include "esw/channel.h"

typedef struct {
    char log[512];
    int32_t vtref_mv;
    uint32_t sw[ESW_GROUP_COUNT];
    uint32_t events[8];
} fake_t;

static const char *gname(esw_group_t g) { return g == ESW_GROUP_VTREF ? "V" : g == ESW_GROUP_SWD ? "S" : "U"; }

static void f_set(void *ctx, esw_group_t g, uint32_t closed)
{
    fake_t *f = ctx;
    f->sw[g] = closed;
    strcat(f->log, gname(g));
    strcat(f->log, closed ? "1 " : "0 ");
}

static int32_t f_vtref(void *ctx)
{
    fake_t *f = ctx;
    /* the interlock must only read VTref with the sense switch closed */
    return f->sw[ESW_GROUP_VTREF] ? f->vtref_mv : -999;
}

static void f_prep(void *ctx, esw_group_t g)
{
    fake_t *f = ctx;
    strcat(f->log, "p");
    strcat(f->log, gname(g));
    strcat(f->log, " ");
}

static void f_event(void *ctx, const esw_channel_event_t *ev) { ((fake_t *)ctx)->events[ev->kind]++; }

static const esw_channel_ops_t ops = { f_set, f_vtref, f_prep, f_event };
static const esw_channel_cfg_t cfg = { 2800, 3600, 50, 2000 };

static fake_t f;
static esw_channel_t ch;

static void setup(int32_t mv)
{
    memset(&f, 0, sizeof(f));
    f.vtref_mv = mv;
    esw_channel_init(&ch, &ops, &f, &cfg);
    f.log[0] = 0;
}

static void default_isolated(void)
{
    setup(3000);
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 1000);
    CHECK(f.log[0] == 0);   /* no holder: nothing ever closes */
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_VTREF), ESW_GSTATE_OPEN);
}

static void vtref_first_then_settle_then_group(void)
{
    setup(2980);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_UART, ESW_HOLD_HOST_UART));
    esw_channel_tick(&ch, 0);
    CHECK(strcmp(f.log, "V1 ") == 0);
    esw_channel_tick(&ch, 49);                          /* still settling: group stays open */
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_UART), ESW_GSTATE_WAIT_VTREF);
    esw_channel_tick(&ch, 50);
    CHECK(strcmp(f.log, "V1 pU U1 ") == 0);              /* lines made safe, then closed */
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_UART), ESW_GSTATE_CLOSED);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 0);
    CHECK_OK(esw_channel_release(&ch, ESW_GROUP_UART, ESW_HOLD_HOST_UART));
    esw_channel_tick(&ch, 60);
    CHECK(strcmp(f.log, "V1 pU U1 U0 V0 ") == 0);        /* signal group first, VTref last */
}

static void unpowered_dut_never_connected(void)
{
    setup(0);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_SWD, ESW_HOLD_HOST_JOB));
    for (uint32_t t = 0; t < 1000; t += 10)
        esw_channel_tick(&ch, t);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 0);
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_SWD), ESW_GSTATE_WAIT_VTREF);
    CHECK_EQ(f.events[ESW_EV_VTREF_BAD], 1);
    f.vtref_mv = 3300;                                   /* DUT powers up: connects by itself */
    esw_channel_tick(&ch, 1010);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 1);
    CHECK_EQ(f.events[ESW_EV_VTREF_OK], 1);
}

static void out_of_window_refused(void)
{
    setup(1800);   /* 1.8 V DUT on 3.3 V I/O */
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_UART, ESW_HOLD_HOST_UART));
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 100);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 0);
}

static void vtref_drop_opens_at_once(void)
{
    setup(3000);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_SWD, ESW_HOLD_HOST_JOB));
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_UART, ESW_HOLD_HOST_UART));
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 50);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 1);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 1);
    f.vtref_mv = 500;                                    /* DUT supply gone */
    esw_channel_tick(&ch, 52);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 0);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 0);
    CHECK_EQ(f.sw[ESW_GROUP_VTREF], 1);                  /* keeps sensing for the holders */
    f.vtref_mv = 3000;
    esw_channel_tick(&ch, 60);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 1);
}

static void openings_are_counted(void)
{
    /* review 2: a short VTref dip opens and re-closes the group between two samples of its state; the counter is
     * the latch that tells a session it is stale */
    setup(3000);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_SWD, ESW_HOLD_LOCAL));
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 50);
    uint32_t gen = ch.opened[ESW_GROUP_SWD];
    f.vtref_mv = 1000;
    esw_channel_tick(&ch, 52);
    f.vtref_mv = 3000;
    esw_channel_tick(&ch, 54);
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_SWD), ESW_GSTATE_CLOSED);   /* closed again ... */
    CHECK(ch.opened[ESW_GROUP_SWD] != gen);                              /* ... but the dip is on record */
}

static void host_loss_releases_host_holders(void)
{
    setup(3000);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_UART, ESW_HOLD_HOST_UART));
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_SWD, ESW_HOLD_LOCAL));
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 50);
    esw_channel_set_host_present(&ch, 0, 100);
    esw_channel_tick(&ch, 1000);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 1);                   /* grace period */
    esw_channel_tick(&ch, 2100);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 0);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 1);                    /* firmware-local holder survives host loss */
    CHECK_EQ(f.events[ESW_EV_HOST_LOST], 1);
    esw_channel_isolate(&ch);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 0);
    CHECK_EQ(f.sw[ESW_GROUP_VTREF], 0);
}

static void host_returns_within_grace(void)
{
    setup(3000);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_UART, ESW_HOLD_CDC_UART));
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 50);
    esw_channel_set_host_present(&ch, 0, 100);
    esw_channel_tick(&ch, 1500);
    esw_channel_set_host_present(&ch, 1, 1600);
    esw_channel_tick(&ch, 5000);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 1);
    CHECK_ERR(esw_channel_hold(&ch, ESW_GROUP_VTREF, ESW_HOLD_LOCAL), ESW_ERR_ARG);
}

/* Mode "off": an inhibited group opens at once, stays open whatever holds it, does not even close VTref sensing,
 * and survives host loss; lifting it brings the holders back into effect. */
static void inhibit_keeps_group_open(void)
{
    setup(3000);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_SWD, ESW_HOLD_LOCAL));
    esw_channel_tick(&ch, 0);
    esw_channel_tick(&ch, 50);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 1);
    uint32_t gen = ch.opened[ESW_GROUP_SWD];
    CHECK_OK(esw_channel_inhibit(&ch, ESW_GROUP_SWD, 1));
    esw_channel_tick(&ch, 52);
    CHECK_EQ(f.sw[ESW_GROUP_SWD], 0);
    CHECK_EQ(f.sw[ESW_GROUP_VTREF], 0);                  /* nothing wants to close: sensing off too */
    CHECK(ch.opened[ESW_GROUP_SWD] != gen);              /* a session on it is stale */
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_SWD), ESW_GSTATE_OPEN);
    f.log[0] = 0;
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_SWD, ESW_HOLD_HOST_PIN));
    esw_channel_set_host_present(&ch, 0, 100);
    for (uint32_t t = 100; t < 5000; t += 10)
        esw_channel_tick(&ch, t);
    CHECK(f.log[0] == 0);                                /* never touched a switch */
    CHECK_EQ(ch.holders[ESW_GROUP_SWD], ESW_HOLD_LOCAL); /* host loss dropped the pin, kept the local holder */
    CHECK(ch.inhibit & (1u << ESW_GROUP_SWD));           /* ... and the inhibit */
    esw_channel_set_host_present(&ch, 1, 5000);
    CHECK_OK(esw_channel_inhibit(&ch, ESW_GROUP_SWD, 0));
    esw_channel_tick(&ch, 5000);
    esw_channel_tick(&ch, 5050);
    CHECK(strcmp(f.log, "V1 pS S1 ") == 0);              /* back through the full interlock sequence */
    CHECK_ERR(esw_channel_inhibit(&ch, ESW_GROUP_VTREF, 1), ESW_ERR_ARG);
}

/* Mode "on" (host pin): the UART group closes with no stream open, and only on a powered DUT. */
static void pin_needs_vtref(void)
{
    setup(1700);
    CHECK_OK(esw_channel_hold(&ch, ESW_GROUP_UART, ESW_HOLD_HOST_PIN));
    for (uint32_t t = 0; t < 500; t += 10)
        esw_channel_tick(&ch, t);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 0);
    CHECK_EQ(esw_channel_state(&ch, ESW_GROUP_UART), ESW_GSTATE_WAIT_VTREF);
    f.vtref_mv = 3300;
    esw_channel_tick(&ch, 510);
    CHECK_EQ(f.sw[ESW_GROUP_UART], 1);
}

int main(void)
{
    RUN(default_isolated);
    RUN(vtref_first_then_settle_then_group);
    RUN(unpowered_dut_never_connected);
    RUN(out_of_window_refused);
    RUN(vtref_drop_opens_at_once);
    RUN(openings_are_counted);
    RUN(host_loss_releases_host_holders);
    RUN(host_returns_within_grace);
    RUN(inhibit_keeps_group_open);
    RUN(pin_needs_vtref);
    return TT_RESULT();
}
