/* SPDX-License-Identifier: MIT */
#include "esw/adiv5.h"
#include <string.h>

void esw_dap_init(esw_dap_t *d, esw_swd_phy_t phy)
{
    memset(d, 0, sizeof(*d));
    d->phy = phy;
    d->check_sticky = 1;
    d->wait_retries = 100;
}

static esw_err_t xfer(esw_dap_t *d, int apndp, int rnw, uint8_t reg, uint32_t *data)
{
    uint8_t req = esw_swd_request(apndp, rnw, reg);
    for (uint32_t attempt = 0; attempt <= d->wait_retries; attempt++) {
        uint32_t v = rnw ? 0u : *data;
        uint32_t ack = d->phy.ops->transfer(d->phy.ctx, req, &v);
        d->stats.xfers++;
        d->last_ack = ack;
        switch (ack) {
        case ESW_ACK_OK:
            if (rnw)
                *data = v;
            return ESW_OK;
        case ESW_ACK_WAIT:
            d->stats.wait++;
            continue;
        case ESW_ACK_FAULT:
            d->stats.fault++;
            return ESW_ERR_FAULT;
        case ESW_ACK_PARITY:
            d->stats.parity++;
            return ESW_ERR_PARITY;
        default:
            d->stats.noack++;
            return (ack == 0x7u || ack == 0x0u) ? ESW_ERR_NO_ACK : ESW_ERR_PROTOCOL;
        }
    }
    return ESW_ERR_WAIT;
}

esw_err_t esw_dp_read(esw_dap_t *d, uint8_t reg, uint32_t *val)
{
    return xfer(d, 0, 1, reg, val);
}

esw_err_t esw_dp_write(esw_dap_t *d, uint8_t reg, uint32_t val)
{
    esw_err_t e = xfer(d, 0, 0, reg, &val);
    if (reg == ESW_DP_SELECT) {
        d->select = val;
        d->select_valid = (e == ESW_OK);
    }
    return e;
}

static esw_err_t ap_select(esw_dap_t *d, uint8_t ap, uint8_t reg)
{
    uint32_t sel = ((uint32_t)ap << 24) | (reg & 0xF0u);   /* DPBANKSEL stays 0 */
    if (d->select_valid && d->select == sel)
        return ESW_OK;
    return esw_dp_write(d, ESW_DP_SELECT, sel);
}

esw_err_t esw_ap_read(esw_dap_t *d, uint8_t ap, uint8_t reg, uint32_t *val)
{
    uint32_t dummy = 0;
    ESW_TRY(ap_select(d, ap, reg));
    ESW_TRY(xfer(d, 1, 1, reg & 0x0Cu, &dummy));   /* posted: the value arrives with the next read */
    return esw_dp_read(d, ESW_DP_RDBUFF, val);
}

esw_err_t esw_ap_write(esw_dap_t *d, uint8_t ap, uint8_t reg, uint32_t val)
{
    ESW_TRY(ap_select(d, ap, reg));
    return xfer(d, 1, 0, reg & 0x0Cu, &val);
}

esw_err_t esw_dap_connect(esw_dap_t *d, uint32_t *dpidr)
{
    static const uint8_t line_reset[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };   /* 56 ones */
    static const uint8_t jtag_to_swd[2] = { 0x9E, 0xE7 };                               /* 0xE79E, LSB first */
    static const uint8_t idle[1] = { 0x00 };
    d->select_valid = 0;
    d->phy.ops->sequence(d->phy.ctx, line_reset, 56);
    d->phy.ops->sequence(d->phy.ctx, jtag_to_swd, 16);
    d->phy.ops->sequence(d->phy.ctx, line_reset, 56);
    d->phy.ops->sequence(d->phy.ctx, idle, 8);
    uint32_t id = 0;
    ESW_TRY(esw_dp_read(d, ESW_DP_DPIDR, &id));
    d->dpidr = id;
    if (dpidr)
        *dpidr = id;
    return ESW_OK;
}

esw_err_t esw_dap_clear_errors(esw_dap_t *d)
{
    return esw_dp_write(d, ESW_DP_ABORT, ESW_ABORT_CLEAR_ALL);
}

esw_err_t esw_dap_abort(esw_dap_t *d)
{
    return esw_dp_write(d, ESW_DP_ABORT, ESW_ABORT_DAPABORT | ESW_ABORT_CLEAR_ALL);
}

esw_err_t esw_dap_power_up(esw_dap_t *d, uint32_t max_polls)
{
    ESW_TRY(esw_dap_clear_errors(d));
    ESW_TRY(esw_dp_write(d, ESW_DP_SELECT, 0));
    ESW_TRY(esw_dp_write(d, ESW_DP_CTRL_STAT, ESW_CS_CDBGPWRUPREQ | ESW_CS_CSYSPWRUPREQ));
    for (uint32_t i = 0; i < max_polls; i++) {
        uint32_t cs = 0;
        ESW_TRY(esw_dp_read(d, ESW_DP_CTRL_STAT, &cs));
        if ((cs & (ESW_CS_CDBGPWRUPACK | ESW_CS_CSYSPWRUPACK)) == (ESW_CS_CDBGPWRUPACK | ESW_CS_CSYSPWRUPACK))
            return ESW_OK;
    }
    return ESW_ERR_TIMEOUT;
}

esw_err_t esw_dap_power_down(esw_dap_t *d)
{
    ESW_TRY(esw_dp_write(d, ESW_DP_SELECT, 0));
    return esw_dp_write(d, ESW_DP_CTRL_STAT, 0);
}

esw_err_t esw_dap_check_errors(esw_dap_t *d)
{
    uint32_t v = 0, cs = 0;
    esw_err_t e = esw_dp_read(d, ESW_DP_RDBUFF, &v);   /* waits for an outstanding AP access */
    if (e != ESW_OK && e != ESW_ERR_FAULT)
        return e;
    ESW_TRY(esw_dp_read(d, ESW_DP_CTRL_STAT, &cs));
    if (cs & ESW_CS_STICKY_MASK) {
        (void)esw_dap_clear_errors(d);
        return ESW_ERR_FAULT;
    }
    return e;
}

void esw_dap_release(esw_dap_t *d)
{
    d->phy.ops->release(d->phy.ctx);
    d->select_valid = 0;
}

/* ---- MEM-AP --------------------------------------------------------------------------------------------- */

void esw_memap_init(esw_memap_t *m, esw_dap_t *d, uint8_t ap, uint32_t csw_base)
{
    memset(m, 0, sizeof(*m));
    m->dap = d;
    m->ap = ap;
    m->csw_base = csw_base & ~(0x7u | 0x30u);
}

void esw_memap_invalidate(esw_memap_t *m)
{
    m->csw_valid = 0;
    m->tar_valid = 0;
}

static esw_err_t set_csw(esw_memap_t *m, uint32_t inc)
{
    uint32_t v = m->csw_base | ESW_CSW_SIZE32 | (inc ? ESW_CSW_ADDRINC_SINGLE : 0u);
    if (m->csw_valid && m->csw == v)
        return ESW_OK;
    m->csw_valid = 0;
    ESW_TRY(esw_ap_write(m->dap, m->ap, ESW_AP_CSW, v));
    m->csw = v;
    m->csw_valid = 1;
    return ESW_OK;
}

static esw_err_t set_tar(esw_memap_t *m, uint32_t addr)
{
    if (m->tar_valid && m->tar == addr)
        return ESW_OK;
    m->tar_valid = 0;
    ESW_TRY(esw_ap_write(m->dap, m->ap, ESW_AP_TAR, addr));
    m->tar = addr;
    m->tar_valid = 1;
    return ESW_OK;
}

static esw_err_t fail(esw_memap_t *m, esw_err_t e)
{
    esw_memap_invalidate(m);
    if (e == ESW_ERR_FAULT)
        (void)esw_dap_clear_errors(m->dap);
    return e;
}

esw_err_t esw_mem_read32(esw_memap_t *m, uint32_t addr, uint32_t *val)
{
    esw_err_t e;
    if ((addr & 3u) != 0)
        return ESW_ERR_ARG;
    if ((e = set_csw(m, 0)) != ESW_OK || (e = set_tar(m, addr)) != ESW_OK)
        return fail(m, e);
    if ((e = esw_ap_read(m->dap, m->ap, ESW_AP_DRW, val)) != ESW_OK)
        return fail(m, e);
    if (m->dap->check_sticky && (e = esw_dap_check_errors(m->dap)) != ESW_OK)
        return fail(m, e);
    return ESW_OK;
}

esw_err_t esw_mem_write32(esw_memap_t *m, uint32_t addr, uint32_t val)
{
    esw_err_t e;
    if ((addr & 3u) != 0)
        return ESW_ERR_ARG;
    if ((e = set_csw(m, 0)) != ESW_OK || (e = set_tar(m, addr)) != ESW_OK)
        return fail(m, e);
    if ((e = esw_ap_write(m->dap, m->ap, ESW_AP_DRW, val)) != ESW_OK)
        return fail(m, e);
    if (m->dap->check_sticky && (e = esw_dap_check_errors(m->dap)) != ESW_OK)
        return fail(m, e);
    return ESW_OK;
}

/* Words until the next 1 KB boundary: auto-increment is only guaranteed inside 1 KB (ADIv5). */
static uint32_t chunk_words(uint32_t addr, uint32_t left)
{
    uint32_t room = (0x400u - (addr & 0x3FFu)) / 4u;
    return left < room ? left : room;
}

esw_err_t esw_mem_read_block(esw_memap_t *m, uint32_t addr, uint32_t *buf, uint32_t nwords)
{
    esw_err_t e;
    if ((addr & 3u) != 0)
        return ESW_ERR_ARG;
    if ((e = set_csw(m, 1)) != ESW_OK)
        return fail(m, e);
    while (nwords) {
        uint32_t n = chunk_words(addr, nwords);
        if ((e = set_tar(m, addr)) != ESW_OK || (e = ap_select(m->dap, m->ap, ESW_AP_DRW)) != ESW_OK)
            return fail(m, e);
        m->tar_valid = 0;   /* TAR moves with every access */
        /* Pipelined posted reads: read k returns word k-1 and starts word k; RDBUFF returns the last word. */
        uint32_t stale = 0;
        if ((e = xfer(m->dap, 1, 1, ESW_AP_DRW, &stale)) != ESW_OK)
            return fail(m, e);
        for (uint32_t i = 0; i + 1u < n; i++) {
            if ((e = xfer(m->dap, 1, 1, ESW_AP_DRW, &buf[i])) != ESW_OK)
                return fail(m, e);
        }
        if ((e = esw_dp_read(m->dap, ESW_DP_RDBUFF, &buf[n - 1u])) != ESW_OK)
            return fail(m, e);
        addr += n * 4u;
        buf += n;
        nwords -= n;
    }
    if ((e = esw_dap_check_errors(m->dap)) != ESW_OK)
        return fail(m, e);
    return ESW_OK;
}

esw_err_t esw_mem_write_block(esw_memap_t *m, uint32_t addr, const uint32_t *buf, uint32_t nwords)
{
    esw_err_t e;
    if ((addr & 3u) != 0)
        return ESW_ERR_ARG;
    if ((e = set_csw(m, 1)) != ESW_OK)
        return fail(m, e);
    while (nwords) {
        uint32_t n = chunk_words(addr, nwords);
        if ((e = set_tar(m, addr)) != ESW_OK)
            return fail(m, e);
        m->tar_valid = 0;
        for (uint32_t i = 0; i < n; i++) {
            if ((e = esw_ap_write(m->dap, m->ap, ESW_AP_DRW, buf[i])) != ESW_OK)
                return fail(m, e);
        }
        addr += n * 4u;
        buf += n;
        nwords -= n;
    }
    if ((e = esw_dap_check_errors(m->dap)) != ESW_OK)
        return fail(m, e);
    return ESW_OK;
}

esw_err_t esw_mem_poll32(esw_memap_t *m, uint32_t addr, uint32_t mask, uint32_t value, uint32_t max_polls,
                         uint32_t *last)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < max_polls; i++) {
        ESW_TRY(esw_mem_read32(m, addr, &v));
        if ((v & mask) == value) {
            if (last)
                *last = v;
            return ESW_OK;
        }
    }
    if (last)
        *last = v;
    return ESW_ERR_TIMEOUT;
}
