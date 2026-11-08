/* SPDX-License-Identifier: MIT */
/* The ESP32-S3 dedicated-GPIO packet sequencer (ports/esp32s3/dgpio_packet.h) against the bit-level simulator:
 * the exact code that runs on the chip, with the platform macros mapped onto the simulated pins. */
#include "tinytest.h"
#include "rig.h"

static swd_sim_t *g_sim;
static uint32_t g_cc, g_stall_at, g_stalled, g_last_edge_cc, g_min_phase, g_track;

#define T_DIO_M 0x1u   /* SWDIO on port bit 0, SWCLK on bit 1: the offsets a first bundle gets */
#define T_CLK_M 0x2u

static uint32_t t_cc(void)
{
    if (g_stall_at && ++g_cc == g_stall_at) {
        g_cc += 1000;   /* an interrupt / stall inside the packet */
        g_stalled = 1;
    }
    else if (!g_stall_at)
        ++g_cc;
    return g_cc;
}

static void t_out(uint32_t v, uint32_t m)
{
    if ((m & T_CLK_M) && g_track) {   /* record the shortest SWCLK phase after the stall */
        uint32_t lvl = (v & T_CLK_M) ? 1u : 0u;
        if (lvl != g_sim->clk) {
            /* every SWCLK phase after the stall (the stalled one is long, not short) */
            if (g_stalled && g_last_edge_cc && g_cc - g_last_edge_cc < g_min_phase)
                g_min_phase = g_cc - g_last_edge_cc;
            g_last_edge_cc = g_cc;
        }
    }
    if (m & T_DIO_M)
        swd_sim_pins.swdio(g_sim, (v & T_DIO_M) ? 1u : 0u);
    if (m & T_CLK_M)
        swd_sim_pins.swclk(g_sim, (v & T_CLK_M) ? 1u : 0u);
}

#define DG_INLINE inline
#define DG_OUT(v, m) t_out((v), (m))
#define DG_IN() (swd_sim_pins.swdio_read(g_sim) << 0)
#define DG_CC() t_cc()
#define DG_OE(e, on) do { swd_sim_pins.swdio_drive(g_sim, (on) ? 1u : 0u); (e)->driving = (on) ? 1u : 0u; } while (0)
#define DG_IRQ_SAVE(e) 0u
#define DG_IRQ_RESTORE(e, s) ((void)(s))
#include "dgpio_packet.h"

static dg_t g_dg;

static uint32_t p_transfer(void *ctx, uint8_t req, uint32_t *data) { return dg_transfer(ctx, req, data); }
static void p_sequence(void *ctx, const uint8_t *bits, uint32_t n) { dg_sequence(ctx, bits, n); }
static void p_release(void *ctx) { dg_release(ctx); }
static const esw_swd_phy_ops_t dg_ops = { p_transfer, p_sequence, p_release, NULL };

static rig_t *setup(uint32_t data_phase)
{
    rig_t *r = rig_init(0);
    g_sim = &r->sim;
    g_cc = 0;
    g_dg = (dg_t){ .clk_m = T_CLK_M, .dio_m = T_DIO_M, .both_m = T_CLK_M | T_DIO_M, .dio_sh = 0, .in_sh = 0,
                   .half = 3, .turnaround = 1, .idle_cycles = 2, .data_phase = (uint8_t)data_phase };
    esw_swd_phy_t phy = { &dg_ops, &g_dg };
    esw_dap_init(&r->dap, phy);   /* replace the rig's bit-bang PHY */
    return r;
}

static void connect_and_aps(void)
{
    rig_t *r = setup(0);
    uint32_t id = 0, idr = 0;
    CHECK_OK(esw_dap_connect(&r->dap, &id));
    CHECK_EQ(id, 0x2BA01477u);
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    CHECK_OK(esw_ap_read(&r->dap, 1, ESW_AP_IDR, &idr));
    CHECK_EQ(idr, 0x02880000u);
    CHECK_EQ(r->sim.contention, 0);
}

static void wait_fault_noack_parity(void)
{
    rig_t *r = setup(0);
    uint32_t v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    r->sim.inject_wait = 7;
    CHECK_OK(esw_ap_read(&r->dap, 0, ESW_AP_IDR, &v));
    CHECK_EQ(v, 0x24770011u);
    CHECK_EQ(r->dap.stats.wait, 7);
    r->sim.inject_parity = 1;
    CHECK_ERR(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v), ESW_ERR_PARITY);
    esw_memap_t m;
    esw_memap_init(&m, &r->dap, 0, ESW_CSW_CORTEXM_PROT);
    CHECK_ERR(esw_mem_read32(&m, 0x30000000u, &v), ESW_ERR_FAULT);   /* bus fault -> sticky -> FAULT */
    CHECK_OK(esw_mem_read32(&m, 0x20000000u, &v));
    r->sim.absent = 1;
    CHECK_ERR(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v), ESW_ERR_NO_ACK);
    r->sim.absent = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_EQ(r->sim.contention, 0);
}

static void data_phase_variant(void)
{
    rig_t *r = setup(1);
    uint32_t v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    r->sim.inject_wait = 3;
    CHECK_OK(esw_ap_read(&r->dap, 0, ESW_AP_IDR, &v));
    CHECK_EQ(v, 0x24770011u);
    CHECK_EQ(r->sim.contention, 0);
}

static void blocks_and_flash(void)
{
    static uint32_t out[600], back[600];
    rig_t *r = setup(0);
    esw_memap_t m;
    for (uint32_t i = 0; i < 600; i++) out[i] = 0xC0DE0000u ^ (i * 2246822519u);
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    esw_memap_init(&m, &r->dap, 0, ESW_CSW_CORTEXM_PROT);
    CHECK_OK(esw_mem_write_block(&m, 0x200003F0u, out, 600));
    CHECK_OK(esw_mem_read_block(&m, 0x200003F0u, back, 600));
    CHECK(memcmp(out, back, sizeof(out)) == 0);
    /* NVMC programming paced by the stall (WAIT) through this engine */
    CHECK_OK(esw_mem_write32(&m, 0x4001E504u, 1));
    CHECK_OK(esw_mem_write_block(&m, 0x1000u, out, 600));
    CHECK_OK(esw_mem_write32(&m, 0x4001E504u, 0));
    CHECK(memcmp(&r->sim.flash[0x1000 / 4], out, sizeof(out)) == 0);
    CHECK(r->sim.stalls > 0);
    CHECK_EQ(r->sim.nvmc_busy_reads, 0);
    CHECK_EQ(r->sim.contention, 0);
}

static void stall_stretches_only_one_phase(void)
{
    rig_t *r = setup(0);
    uint32_t v = 0;
    g_dg.half = 20;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    g_stall_at = g_cc + 300;   /* lands inside the next packet */
    g_stalled = 0;
    g_track = 1;
    g_min_phase = 0xFFFFFFFFu;
    g_last_edge_cc = 0;
    for (int i = 0; i < 20; i++)
        CHECK_OK(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v));
    g_track = 0;
    g_stall_at = 0;
    CHECK(g_min_phase != 0xFFFFFFFFu);
    CHECK_EQ(g_stalled, 1);
    CHECK(g_min_phase >= g_dg.half - 2u);   /* no back-to-back catch-up edges after the stall (2 calls of slack) */
}

static void sequence_and_release(void)
{
    rig_t *r = setup(0);
    static const uint8_t ones[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    dg_sequence(&g_dg, ones, 56);   /* a line reset locks the DP out until DPIDR is read */
    uint32_t v = 0;
    CHECK_ERR(esw_dp_read(&r->dap, ESW_DP_CTRL_STAT, &v), ESW_ERR_NO_ACK);
    CHECK_OK(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v));
    dg_release(&g_dg);
    CHECK_EQ(r->sim.host_drives, 0);
    CHECK_EQ(r->sim.clk, 0);
}

int main(void)
{
    RUN(connect_and_aps);
    RUN(wait_fault_noack_parity);
    RUN(data_phase_variant);
    RUN(blocks_and_flash);
    RUN(stall_stretches_only_one_phase);
    RUN(sequence_and_release);
    return TT_RESULT();
}
