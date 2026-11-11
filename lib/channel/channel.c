/* SPDX-License-Identifier: MIT */
#include "esw/channel.h"
#include <string.h>

static void emit(esw_channel_t *c, esw_ev_kind_t kind, esw_group_t g, esw_ev_why_t why)
{
    if (c->ops->event) {
        esw_channel_event_t ev = { kind, g, why, c->vtref_mv };
        c->ops->event(c->ctx, &ev);
    }
}

void esw_channel_init(esw_channel_t *c, const esw_channel_ops_t *ops, void *ctx, const esw_channel_cfg_t *cfg)
{
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->ctx = ctx;
    c->cfg = *cfg;
    c->host_present = 1;
    c->host_loss_done = 1;
    /* signal groups first, VTref last */
    ops->set_switch(ctx, ESW_GROUP_SWD, 0);
    ops->set_switch(ctx, ESW_GROUP_UART, 0);
    ops->set_switch(ctx, ESW_GROUP_VTREF, 0);
}

esw_err_t esw_channel_hold(esw_channel_t *c, esw_group_t g, uint32_t holder)
{
    if (g == ESW_GROUP_VTREF || (unsigned)g >= ESW_GROUP_COUNT || !holder)
        return ESW_ERR_ARG;
    c->holders[g] |= holder;
    return ESW_OK;
}

esw_err_t esw_channel_release(esw_channel_t *c, esw_group_t g, uint32_t holder)
{
    if (g == ESW_GROUP_VTREF || (unsigned)g >= ESW_GROUP_COUNT)
        return ESW_ERR_ARG;
    c->holders[g] &= ~holder;
    return ESW_OK;
}

esw_err_t esw_channel_inhibit(esw_channel_t *c, esw_group_t g, uint32_t on)
{
    if (g == ESW_GROUP_VTREF || (unsigned)g >= ESW_GROUP_COUNT)
        return ESW_ERR_ARG;
    if (on)
        c->inhibit |= 1u << g;
    else
        c->inhibit &= ~(1u << g);
    return ESW_OK;
}

static uint32_t wants(const esw_channel_t *c, uint32_t g)
{
    return c->holders[g] != 0 && !(c->inhibit & (1u << g));
}

void esw_channel_set_host_present(esw_channel_t *c, uint32_t present, uint32_t now_ms)
{
    if (present) {
        c->host_present = 1;
        c->host_loss_done = 1;
    } else if (c->host_present) {
        c->host_present = 0;
        c->host_loss_done = 0;
        c->host_lost_ms = now_ms;
    }
}

static void open_group(esw_channel_t *c, esw_group_t g, esw_ev_why_t why)
{
    c->ops->set_switch(c->ctx, g, 0);
    c->gstate[g] = ESW_GSTATE_OPEN;
    c->opened[g]++;
    emit(c, ESW_EV_GROUP_OPENED, g, why);
}

void esw_channel_tick(esw_channel_t *c, uint32_t now_ms)
{
    /* 1. host loss: drop host-owned holders after the grace period */
    if (!c->host_present && !c->host_loss_done && now_ms - c->host_lost_ms >= c->cfg.host_loss_grace_ms) {
        c->host_loss_done = 1;
        c->holders[ESW_GROUP_SWD] &= ~ESW_HOLD_HOST_MASK;
        c->holders[ESW_GROUP_UART] &= ~ESW_HOLD_HOST_MASK;
        emit(c, ESW_EV_HOST_LOST, ESW_GROUP_VTREF, ESW_WHY_HOST_LOSS);
    }

    /* 2. VTref sensing follows the groups that want to close */
    uint32_t need = wants(c, ESW_GROUP_SWD) | wants(c, ESW_GROUP_UART);
    if (need && !c->vtref_closed) {
        c->ops->set_switch(c->ctx, ESW_GROUP_VTREF, 1);
        c->vtref_closed = 1;
        c->vtref_valid = 0;
        c->vtref_ok = 0;
        c->vtref_since_ms = now_ms;
    }
    if (c->vtref_closed && now_ms - c->vtref_since_ms >= c->cfg.vtref_settle_ms) {
        c->vtref_mv = c->ops->read_vtref_mv(c->ctx);
        uint8_t ok = c->vtref_mv >= c->cfg.vtref_min_mv && c->vtref_mv <= c->cfg.vtref_max_mv;
        if (!c->vtref_valid || ok != c->vtref_ok)
            emit(c, ok ? ESW_EV_VTREF_OK : ESW_EV_VTREF_BAD, ESW_GROUP_VTREF, ESW_WHY_VTREF);
        c->vtref_valid = 1;
        c->vtref_ok = ok;
    }

    /* 3. signal groups: close only with a holder and a valid VTref; open at once otherwise */
    for (uint32_t g = ESW_GROUP_SWD; g < ESW_GROUP_COUNT; g++) {
        uint32_t want = wants(c, g);
        uint32_t powered = c->vtref_valid && c->vtref_ok;
        if (c->gstate[g] == ESW_GSTATE_CLOSED) {
            if (!want)
                open_group(c, (esw_group_t)g, (c->inhibit & (1u << g)) ? ESW_WHY_FORCED
                                              : c->host_loss_done && !c->host_present ? ESW_WHY_HOST_LOSS
                                                                                      : ESW_WHY_HOLDER);
            else if (!powered) {
                open_group(c, (esw_group_t)g, ESW_WHY_VTREF);
                c->gstate[g] = ESW_GSTATE_WAIT_VTREF;
            }
        } else if (want) {
            if (powered) {
                c->ops->prepare_lines(c->ctx, (esw_group_t)g);
                c->ops->set_switch(c->ctx, (esw_group_t)g, 1);
                c->gstate[g] = ESW_GSTATE_CLOSED;
                emit(c, ESW_EV_GROUP_CLOSED, (esw_group_t)g, ESW_WHY_HOLDER);
            } else {
                c->gstate[g] = ESW_GSTATE_WAIT_VTREF;
            }
        } else {
            c->gstate[g] = ESW_GSTATE_OPEN;
        }
    }

    /* 4. VTref opens last, once no group needs it */
    if (!need && c->vtref_closed) {
        c->ops->set_switch(c->ctx, ESW_GROUP_VTREF, 0);
        c->vtref_closed = 0;
        c->vtref_valid = 0;
        c->vtref_ok = 0;
    }
}

void esw_channel_isolate(esw_channel_t *c)
{
    c->holders[ESW_GROUP_SWD] = 0;
    c->holders[ESW_GROUP_UART] = 0;
    for (uint32_t g = ESW_GROUP_SWD; g < ESW_GROUP_COUNT; g++) {
        if (c->gstate[g] == ESW_GSTATE_CLOSED)
            open_group(c, (esw_group_t)g, ESW_WHY_FORCED);
        c->gstate[g] = ESW_GSTATE_OPEN;
    }
    c->ops->set_switch(c->ctx, ESW_GROUP_VTREF, 0);
    c->vtref_closed = 0;
    c->vtref_valid = 0;
    c->vtref_ok = 0;
}

esw_gstate_t esw_channel_state(const esw_channel_t *c, esw_group_t g)
{
    if (g == ESW_GROUP_VTREF)
        return c->vtref_closed ? ESW_GSTATE_CLOSED : ESW_GSTATE_OPEN;
    return c->gstate[g];
}
