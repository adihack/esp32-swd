/* SPDX-License-Identifier: MIT */
/* SWD packet engine, ADIv5 DP/AP, MEM-AP and Cortex-M core control against the simulator. */
#include "tinytest.h"
#include "rig.h"
#include "esw/cortexm.h"

static void connect_reads_dpidr(void)
{
    rig_t *r = rig_init(0);
    uint32_t id = 0;
    CHECK_OK(esw_dap_connect(&r->dap, &id));
    CHECK_EQ(id, 0x2BA01477u);
    CHECK_EQ(r->sim.contention, 0);
}

static void no_target_is_no_ack(void)
{
    rig_t *r = rig_init(0);
    r->sim.absent = 1;
    CHECK_ERR(esw_dap_connect(&r->dap, NULL), ESW_ERR_NO_ACK);
}

static void lockout_until_dpidr(void)
{
    rig_t *r = rig_init(0);
    uint32_t v = 0;
    CHECK_ERR(esw_dp_read(&r->dap, ESW_DP_CTRL_STAT, &v), ESW_ERR_NO_ACK);   /* never connected: locked out */
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dp_read(&r->dap, ESW_DP_CTRL_STAT, &v));
}

static void power_up_and_ap_idr(void)
{
    rig_t *r = rig_init(0);
    uint32_t idr0 = 0, idr1 = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    CHECK_OK(esw_ap_read(&r->dap, 0, ESW_AP_IDR, &idr0));
    CHECK_OK(esw_ap_read(&r->dap, 1, ESW_AP_IDR, &idr1));
    CHECK_EQ(idr0, 0x24770011u);
    CHECK_EQ(idr1, 0x02880000u);
    CHECK_EQ(r->sim.contention, 0);
}

static void wait_is_retried_then_exhausted(void)
{
    rig_t *r = rig_init(0);
    uint32_t v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    r->sim.inject_wait = 5;
    CHECK_OK(esw_ap_read(&r->dap, 0, ESW_AP_IDR, &v));
    CHECK_EQ(v, 0x24770011u);
    CHECK_EQ(r->dap.stats.wait, 5);
    r->sim.inject_wait = 1000;
    CHECK_ERR(esw_ap_read(&r->dap, 0, ESW_AP_IDR, &v), ESW_ERR_WAIT);
    r->sim.inject_wait = 0;
    CHECK_OK(esw_dap_abort(&r->dap));
    CHECK_OK(esw_ap_read(&r->dap, 0, ESW_AP_IDR, &v));
}

static void parity_error_detected(void)
{
    rig_t *r = rig_init(0);
    uint32_t v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    r->sim.inject_parity = 1;
    CHECK_ERR(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v), ESW_ERR_PARITY);
    CHECK_OK(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v));
    CHECK_EQ(v, 0x2BA01477u);
}

static void mem_rw_and_bus_fault_recovery(void)
{
    rig_t *r = rig_init(0);
    esw_memap_t m;
    uint32_t v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    esw_memap_init(&m, &r->dap, 0, ESW_CSW_CORTEXM_PROT);
    CHECK_OK(esw_mem_write32(&m, 0x20000010u, 0xCAFEF00Du));
    CHECK_OK(esw_mem_read32(&m, 0x20000010u, &v));
    CHECK_EQ(v, 0xCAFEF00Du);
    CHECK_EQ(r->sim.ram[4], 0xCAFEF00Du);
    CHECK_ERR(esw_mem_read32(&m, 0x30000000u, &v), ESW_ERR_FAULT);   /* unmapped: sticky error */
    CHECK_OK(esw_mem_read32(&m, 0x20000010u, &v));                    /* cleared and usable again */
    CHECK_EQ(v, 0xCAFEF00Du);
    CHECK_ERR(esw_mem_read32(&m, 0x20000011u, &v), ESW_ERR_ARG);
}

static void block_crosses_1k_boundaries(void)
{
    static uint32_t out[300], back[300];
    rig_t *r = rig_init(0);
    esw_memap_t m;
    for (uint32_t i = 0; i < 300; i++)
        out[i] = 0x5A000000u ^ (i * 2654435761u);
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    esw_memap_init(&m, &r->dap, 0, ESW_CSW_CORTEXM_PROT);
    /* 0x200003C0 + 1200 bytes crosses 0x20000400 and 0x20000800; the sim wraps TAR inside 1 KB like hardware may */
    uint32_t before = r->dap.stats.xfers;
    CHECK_OK(esw_mem_write_block(&m, 0x200003C0u, out, 300));
    uint32_t wr_packets = r->dap.stats.xfers - before;
    for (uint32_t i = 0; i < 300; i++)
        CHECK_EQ(r->sim.ram[(0x3C0u / 4) + i], out[i]);
    before = r->dap.stats.xfers;
    CHECK_OK(esw_mem_read_block(&m, 0x200003C0u, back, 300));
    uint32_t rd_packets = r->dap.stats.xfers - before;
    for (uint32_t i = 0; i < 300; i++)
        CHECK_EQ(back[i], out[i]);
    /* about one packet per word: 300 data + per-chunk TAR/select + final sticky check */
    CHECK(wr_packets <= 300 + 12);
    CHECK(rd_packets <= 300 + 15);
    CHECK_EQ(r->sim.contention, 0);
}

static void swdio_reconfigured_behind_engine(void)
{
    /* review finding: the interlock's line preparation disabled SWDIO output while the engine still believed it was
     * driving; every later packet then went out with SWDIO released (NO_ACK) */
    rig_t *r = rig_init(0);
    uint32_t v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    r->sim.host_drives = 0;   /* external reconfiguration, engine cache says driving */
    CHECK_OK(esw_dp_read(&r->dap, ESW_DP_DPIDR, &v));
    CHECK_EQ(v, 0x2BA01477u);
}

static void cortexm_halt_regs_resume(void)
{
    rig_t *r = rig_init(0);
    esw_memap_t m;
    esw_cpu_t cpu;
    uint32_t halted = 0, v = 0;
    CHECK_OK(esw_dap_connect(&r->dap, NULL));
    CHECK_OK(esw_dap_power_up(&r->dap, 10));
    esw_memap_init(&m, &r->dap, 0, ESW_CSW_CORTEXM_PROT);
    esw_cortexm_init(&cpu, &m);
    CHECK_OK(cpu.ops->halt(&cpu));
    CHECK_OK(cpu.ops->is_halted(&cpu, &halted));
    CHECK_EQ(halted, 1);
    CHECK_OK(cpu.ops->reg_write(&cpu, ESW_REG_ARG0, 0x12345678u));
    CHECK_OK(cpu.ops->reg_read(&cpu, ESW_REG_ARG0, &v));
    CHECK_EQ(v, 0x12345678u);
    CHECK_EQ(r->sim.regs[0], 0x12345678u);
    CHECK_OK(cpu.ops->resume(&cpu));
    CHECK_OK(cpu.ops->is_halted(&cpu, &halted));
    CHECK_EQ(halted, 0);
}

int main(void)
{
    RUN(connect_reads_dpidr);
    RUN(no_target_is_no_ack);
    RUN(lockout_until_dpidr);
    RUN(power_up_and_ap_idr);
    RUN(wait_is_retried_then_exhausted);
    RUN(parity_error_detected);
    RUN(mem_rw_and_bus_fault_recovery);
    RUN(block_crosses_1k_boundaries);
    RUN(swdio_reconfigured_behind_engine);
    RUN(cortexm_halt_regs_resume);
    return TT_RESULT();
}
