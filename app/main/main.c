/* SPDX-License-Identifier: MIT */
/* esp-swd test firmware for r5 (milestone M1 preview).
 *
 * Link: the ESP32-S3 USB-Serial-JTAG CDC port (no TinyUSB yet; esptool keeps working without a BOOT button),
 * framed with lib/proto (COBS + CRC-16). Tasks:
 *   esw_tick  (core 0, every 2 ms): isolation interlock for both channels, host-presence lease, status LED
 *   esw_proto (core 1):             command dispatcher; SWD runs here, undisturbed by the tick task
 * Logs go to UART0 (GPIO43, J201 is DNP), so nothing else writes to the USB link.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_ota_ops.h"
#include "esp_attr.h"
#include "esp_brownout.h"
#include "esp_log.h"
#include "esw/proto.h"
#include "esw/channel.h"
#include "esw/adiv5.h"
#include "esw/swd_bitbang.h"
#include "esw/target.h"
#include "esw_port.h"
#include "esw_dgpio.h"
#include "board_r5.h"

#define FW_NAME "esp-swd 0.1.0 r5/usj"
#define HOST_LEASE_MS 3000u      /* no valid frame for this long = host gone (then the channel grace period runs) */
#define MAX_WORDS 512u

typedef struct {
    esw_port_swd_t pins;
    esw_bitbang_t bb;
    esw_dap_t dap;
    esw_target_t tgt;
    uint8_t attached;
    uint8_t phy;           /* 0 portable bit-bang, 1 ESP32-S3 dedicated GPIO */
    uint32_t hz;           /* dgpio: actual SWCLK */
    uint32_t gen;          /* channel.opened[SWD] at attach: any opening since then makes the session stale */
    uint32_t last_op_us;
} session_t;

static esw_channel_t g_ch[BOARD_NUM_CHANNELS];
static esw_dgpio_t g_dg[BOARD_NUM_CHANNELS];   /* created at boot on core 1 (proto task), never deleted */
static uint8_t g_dg_ok[BOARD_NUM_CHANNELS];
static SemaphoreHandle_t g_lock;
static session_t g_s[BOARD_NUM_CHANNELS];
static volatile uint32_t g_last_rx_ms, g_busy;
static esw_frame_rx_t g_rx;
static uint8_t g_req[ESW_PROTO_MAX_PAYLOAD], g_rsp[ESW_PROTO_MAX_PAYLOAD], g_frame[ESW_FRAME_MAX], g_usb[512];
static uint32_t g_words[MAX_WORDS];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static esw_gstate_t group_state(int ch, esw_group_t g)
{
    esw_gstate_t st = ESW_GSTATE_OPEN;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        st = esw_channel_state(&g_ch[ch], g);
        xSemaphoreGive(g_lock);
    }
    return st;
}

/* ---- interlock tick ------------------------------------------------------------------------------------ */
static void tick_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t n = 0;
    for (;;) {
        uint32_t t = now_ms();
        uint32_t present = g_busy || (usb_serial_jtag_is_connected() && t - g_last_rx_ms < HOST_LEASE_MS);
        if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50))) {
            for (int i = 0; i < BOARD_NUM_CHANNELS; i++) {
                esw_channel_set_host_present(&g_ch[i], present, t);
                esw_channel_tick(&g_ch[i], t);
            }
            xSemaphoreGive(g_lock);
        }
        board_led((n++ % 500u) < 50u);   /* 100 ms flash every second */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(2));
    }
}

/* ---- SWD session ---------------------------------------------------------------------------------------- */
/* An attached session holds the SWD group itself (ESW_HOLD_LOCAL): host loss alone never opens the switch under a
 * live session. The proto task first ends the session cleanly (debug off, pin reset) and only then lets go. */
static void session_hold(int ch, uint32_t on)
{
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        if (on)
            (void)esw_channel_hold(&g_ch[ch], ESW_GROUP_SWD, ESW_HOLD_LOCAL);
        else
            (void)esw_channel_release(&g_ch[ch], ESW_GROUP_SWD, ESW_HOLD_LOCAL);
        xSemaphoreGive(g_lock);
    }
}

static void detach(int ch)
{
    session_t *s = &g_s[ch];
    if (!s->attached)
        return;
    (void)esw_target_finish(&s->tgt);
    if (s->tgt.drv && s->tgt.drv->detach)
        (void)s->tgt.drv->detach(&s->tgt);
    (void)esw_dap_power_down(&s->dap);
    esw_dap_release(&s->dap);
    if (group_state(ch, ESW_GROUP_SWD) == ESW_GSTATE_CLOSED)
        board_nrst_pulse(&board_ch[ch], 10);   /* e.g. nRF52 leaves debug interface mode only on a reset */
    s->attached = 0;
    session_hold(ch, 0);
}

/* The switch opened under a live session (VTref lost): the wire is gone, so drop the session without touching
 * the target. The host must attach again (the DUT may have browned out). */
static void drop_session(int ch)
{
    session_t *s = &g_s[ch];
    esw_dap_release(&s->dap);   /* tool-side lines only */
    s->attached = 0;
    session_hold(ch, 0);
}

/* The session still sits on the same closed switch: the SWD group is closed and has not opened since attach (a
 * VTref dip can open and re-close it between two looks at its state; the opening counter latches it). A lock
 * timeout counts as "not ok": the session is dropped, the conservative side. */
static uint32_t session_wire_ok(int ch)
{
    uint32_t ok = 0;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        ok = esw_channel_state(&g_ch[ch], ESW_GROUP_SWD) == ESW_GSTATE_CLOSED &&
             g_ch[ch].opened[ESW_GROUP_SWD] == g_s[ch].gen;
        xSemaphoreGive(g_lock);
    }
    return ok;
}

/* Host loss as decided by the channel itself (lease + grace period): one clock for both decisions. */
static uint32_t host_lost(int ch)
{
    uint32_t lost = 0;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        lost = !g_ch[ch].host_present && g_ch[ch].host_loss_done;
        xSemaphoreGive(g_lock);
    }
    return lost;
}

/* Called by the proto task every loop (<= 20 ms). */
static void supervise(void)
{
    for (int ch = 0; ch < BOARD_NUM_CHANNELS; ch++) {
        if (!g_s[ch].attached)
            continue;
        if (!session_wire_ok(ch))
            drop_session(ch);
        else if (host_lost(ch))
            detach(ch);   /* clean end while the switch is still closed; then the group opens */
    }
}

static esw_err_t attach(int ch, uint32_t clock, uint32_t phy_kind)
{
    session_t *s = &g_s[ch];
    detach(ch);
    /* Check and take the session holder in one critical section, and only while the host itself holds SWD: the
     * group state lags the holders by one tick, so CLOSED alone could belong to a release in flight. */
    uint32_t ok = 0;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        ok = esw_channel_state(&g_ch[ch], ESW_GROUP_SWD) == ESW_GSTATE_CLOSED &&
             (g_ch[ch].holders[ESW_GROUP_SWD] & ESW_HOLD_HOST_MASK) != 0;
        if (ok) {
            s->gen = g_ch[ch].opened[ESW_GROUP_SWD];
            (void)esw_channel_hold(&g_ch[ch], ESW_GROUP_SWD, ESW_HOLD_LOCAL);
        }
        xSemaphoreGive(g_lock);
    }
    if (!ok)
        return ESW_ERR_INTERLOCK;
    memset(&s->tgt, 0, sizeof(s->tgt));
    s->phy = 0;
    s->hz = 0;
    esw_err_t e = ESW_OK;
    if (phy_kind > 1 || (phy_kind == 0 && clock > 10000u)) {   /* 0: busy-loop count, sane range only */
        session_hold(ch, 0);
        return ESW_ERR_ARG;
    }
    if (phy_kind == 1) {
        /* dedicated-GPIO engine: connect at min(requested, 1 MHz), then switch to the requested clock */
        uint32_t connect_hz = (clock && clock < 1000000u) ? clock : 1000000u;
        e = g_dg_ok[ch] ? esw_dgpio_attach(&g_dg[ch], connect_hz) : ESW_ERR_UNSUPPORTED;
        if (e == ESW_OK) {
            esw_dap_init(&s->dap, esw_dgpio_phy(&g_dg[ch]));
            s->dap.wait_retries = 2000;   /* a WAIT packet is ~2 us at 8 MHz: keep ~4 ms of patience */
            s->phy = 1;
        }
    } else {
        esw_port_swd_init(&s->pins, board_ch[ch].swclk, board_ch[ch].swdio, clock);   /* clock = half-period loops */
        esw_bitbang_init(&s->bb, &esw_port_swd_pins, &s->pins);
        esw_dap_init(&s->dap, esw_bitbang_phy(&s->bb));
    }
    if (e != ESW_OK) {
        session_hold(ch, 0);
        return e;
    }
    e = esw_dap_connect(&s->dap, NULL);
    uint32_t connected = (e == ESW_OK);
    if (e == ESW_OK)
        e = esw_dap_power_up(&s->dap, 100);
    if (e == ESW_OK)
        e = esw_target_detect(&s->tgt, &s->dap, &esw_port_clock);
    if (e == ESW_OK) {
        if (s->phy == 1)
            s->hz = esw_dgpio_set_clock(&g_dg[ch], clock);
        s->attached = 1;
        return ESW_OK;
    }
    /* failed after reaching the target (unsupported part, transient fault): leave it as found, i.e. debug domain
     * off and a pin reset (nRF52 leaves debug interface mode only on a reset) */
    if (connected)
        (void)esw_dap_power_down(&s->dap);
    esw_dap_release(&s->dap);
    if (connected && session_wire_ok(ch))
        board_nrst_pulse(&board_ch[ch], 10);
    session_hold(ch, 0);
    return e;
}

/* ---- command dispatcher ------------------------------------------------------------------------------- */
static uint8_t g_reboot = 0xFF;   /* 0xFF: no reboot requested; 0 normal, 1 ROM download */
static uint8_t g_test_panic;

#define NEED(n) do { if (an < (n)) { st = ESW_ERR_ARG; goto done; } } while (0)
#define CHAN() do { NEED(1); ch = a[0]; if (ch >= BOARD_NUM_CHANNELS) { st = ESW_ERR_ARG; goto done; } s = &g_s[ch]; } while (0)
#define ATTACHED() do { if (!s->attached) { st = ESW_ERR_STATE; goto done; } \
                            if (!session_wire_ok(ch)) { drop_session(ch); st = ESW_ERR_STATE; goto done; } } while (0)

static int handle(const uint8_t *q, int n, uint8_t *r)
{
    const uint8_t *a = q + 2;
    int an = n - 2, rl = 3, ch = 0;
    session_t *s = NULL;
    esw_err_t st = ESW_OK;
    r[0] = q[0];
    r[1] = (uint8_t)(q[1] | 0x80u);
    uint32_t t0 = now_us();

    switch (q[1]) {
    case ESW_CMD_PING:
        if (an > (int)sizeof(g_rsp) - 3) {   /* echo must fit the response buffer */
            st = ESW_ERR_ARG;
            break;
        }
        memcpy(r + rl, a, (size_t)an);
        rl += an;
        break;
    case ESW_CMD_INFO:
        wr32(r + 3, ESW_PROTO_VERSION);
        wr32(r + 7, now_ms());
        wr32(r + 11, (uint32_t)esp_reset_reason());
        wr32(r + 15, (uint32_t)esp_get_free_heap_size());
        memset(r + 19, 0, 32);
        strncpy((char *)r + 19, FW_NAME, 31);
        rl = 51;
        break;
    case ESW_CMD_REBOOT:
        NEED(1);
        if (a[0] > 1) {
            st = ESW_ERR_ARG;
            break;
        }
        g_reboot = a[0];
        break;
    case ESW_CMD_CH_HOLD:
    case ESW_CMD_CH_RELEASE: {
        CHAN();
        NEED(2);
        esw_group_t g = (esw_group_t)a[1];
        uint32_t holder = g == ESW_GROUP_UART ? ESW_HOLD_HOST_UART : ESW_HOLD_HOST_JOB;
        if (q[1] == ESW_CMD_CH_RELEASE && g == ESW_GROUP_SWD)
            detach(ch);
        if (!xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
            st = ESW_ERR_TIMEOUT;
            break;
        }
        st = q[1] == ESW_CMD_CH_HOLD ? esw_channel_hold(&g_ch[ch], g, holder)
                                     : esw_channel_release(&g_ch[ch], g, holder);
        xSemaphoreGive(g_lock);
        break;
    }
    case ESW_CMD_CH_STATE:
        CHAN();
        if (!xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
            st = ESW_ERR_TIMEOUT;
            break;
        }
        wr32(r + 3, (uint32_t)g_ch[ch].vtref_mv);
        r[7] = g_ch[ch].vtref_closed;
        r[8] = (uint8_t)esw_channel_state(&g_ch[ch], ESW_GROUP_SWD);
        r[9] = (uint8_t)esw_channel_state(&g_ch[ch], ESW_GROUP_UART);
        r[10] = g_ch[ch].vtref_valid;
        wr32(r + 11, g_ch[ch].holders[ESW_GROUP_SWD]);
        wr32(r + 15, g_ch[ch].holders[ESW_GROUP_UART]);
        xSemaphoreGive(g_lock);
        rl = 19;
        break;
    case ESW_CMD_SWD_ATTACH: {
        CHAN();
        NEED(5);
        st = attach(ch, rd32(a + 1), an >= 6 ? a[5] : 0u);   /* optional phy byte: 0 bit-bang, 1 dgpio */
        esw_target_t *t = &s->tgt;
        wr32(r + 3, s->dap.dpidr);
        r[7] = (uint8_t)t->locked;
        r[8] = (uint8_t)t->nregions;
        r[9] = (uint8_t)((s->hz / 1000u) & 0xFFu);   /* actual SWCLK in kHz (dgpio), 0 for bit-bang */
        r[10] = (uint8_t)((s->hz / 1000u) >> 8);
        memset(r + 11, 0, 32);
        strncpy((char *)r + 11, t->drv ? t->part : "", 31);
        rl = 43;
        for (uint32_t i = 0; i < t->nregions; i++, rl += 16) {
            wr32(r + rl, t->regions[i].start);
            wr32(r + rl + 4, t->regions[i].size);
            wr32(r + rl + 8, t->regions[i].sector_size);
            wr32(r + rl + 12, (uint32_t)t->regions[i].kind);
        }
        break;
    }
    case ESW_CMD_SWD_DETACH:
        CHAN();
        detach(ch);
        break;
    case ESW_CMD_TGT_UNLOCK:
        CHAN();
        ATTACHED();
        NEED(5);
        st = s->tgt.drv->unlock(&s->tgt, rd32(a + 1));
        break;
    case ESW_CMD_TGT_RESET:
        CHAN();
        ATTACHED();
        NEED(5);
        st = s->tgt.drv->reset(&s->tgt, rd32(a + 1));
        break;
    case ESW_CMD_MEM_READ: {
        CHAN();
        ATTACHED();
        NEED(7);
        uint32_t nw = (uint32_t)(a[5] | (a[6] << 8));
        if (nw == 0 || nw > MAX_WORDS) {
            st = ESW_ERR_ARG;
            break;
        }
        st = esw_mem_read_block(&s->tgt.mem, rd32(a + 1), g_words, nw);
        if (st == ESW_OK) {
            memcpy(r + rl, g_words, nw * 4u);
            rl += (int)(nw * 4u);
        }
        break;
    }
    case ESW_CMD_MEM_WRITE:
    case ESW_CMD_TGT_PROGRAM:
    case ESW_CMD_TGT_VERIFY: {
        CHAN();
        ATTACHED();
        NEED(9);
        uint32_t nw = (uint32_t)(an - 5) / 4u, addr = rd32(a + 1);
        if (nw > MAX_WORDS || (an - 5) % 4) {
            st = ESW_ERR_ARG;
            break;
        }
        memcpy(g_words, a + 5, nw * 4u);
        if (q[1] == ESW_CMD_MEM_WRITE)
            st = esw_mem_write_block(&s->tgt.mem, addr, g_words, nw);
        else if (q[1] == ESW_CMD_TGT_PROGRAM)
            st = esw_target_program(&s->tgt, addr, g_words, nw);
        else
            st = esw_target_verify(&s->tgt, addr, g_words, nw);
        break;
    }
    case ESW_CMD_TGT_ERASE:
        CHAN();
        ATTACHED();
        NEED(9);
        st = esw_target_erase(&s->tgt, rd32(a + 1), rd32(a + 5));
        break;
    case ESW_CMD_SWD_BENCH: {
        CHAN();
        ATTACHED();
        NEED(6);
        uint32_t mode = a[1], cnt = rd32(a + 2), errors = 0;
        if (cnt > 10000000u) {   /* bounded: the proto task does nothing else meanwhile */
            st = ESW_ERR_ARG;
            break;
        }
        const uint32_t base = 0x20010000u;   /* target RAM scratch (nRF52840: 256 KB from 0x20000000) */
        esw_dap_stats_t s0 = s->dap.stats;
        uint32_t b0 = now_us();
        if (mode == 0) {
            for (uint32_t i = 0; i < cnt && st == ESW_OK; i++) {
                uint32_t v = 0;
                st = esw_dp_read(&s->dap, ESW_DP_DPIDR, &v);
                errors += (st == ESW_OK && v != s->dap.dpidr);
            }
        } else if (mode == 1 || mode == 2) {
            for (uint32_t done = 0; done < cnt && st == ESW_OK; done += MAX_WORDS) {
                uint32_t n = cnt - done < MAX_WORDS ? cnt - done : MAX_WORDS;
                uint32_t addr = base + ((done * 4u) % 0x20000u);
                if (mode == 1) {
                    for (uint32_t i = 0; i < n; i++)
                        g_words[i] = (addr + 4u * i) ^ 0xA5C3E1F0u;
                    st = esw_mem_write_block(&s->tgt.mem, addr, g_words, n);
                } else {
                    st = esw_mem_read_block(&s->tgt.mem, addr, g_words, n);
                    for (uint32_t i = 0; i < n && st == ESW_OK; i++)
                        errors += g_words[i] != ((addr + 4u * i) ^ 0xA5C3E1F0u);
                }
            }
        } else {
            st = ESW_ERR_ARG;
        }
        wr32(r + 3, now_us() - b0);
        wr32(r + 7, errors);
        wr32(r + 11, s->dap.stats.xfers - s0.xfers);
        wr32(r + 15, s->dap.stats.wait - s0.wait);
        wr32(r + 19, s->dap.stats.fault - s0.fault);
        wr32(r + 23, s->dap.stats.noack - s0.noack);
        wr32(r + 27, s->dap.stats.parity - s0.parity);
        rl = 31;
        break;
    }
    case ESW_CMD_TGT_CRC32: {
        CHAN();
        ATTACHED();
        NEED(13);
        uint32_t crc = 0;
        st = esw_target_crc32(&s->tgt, rd32(a + 1), rd32(a + 5), rd32(a + 9), &crc);
        wr32(r + 3, crc);
        rl = 7;
        break;
    }
    case ESW_CMD_SWD_STATS:
        CHAN();
        wr32(r + 3, s->dap.stats.xfers);
        wr32(r + 7, s->dap.stats.wait);
        wr32(r + 11, s->dap.stats.fault);
        wr32(r + 15, s->dap.stats.noack);
        wr32(r + 19, s->dap.stats.parity);
        wr32(r + 23, s->last_op_us);
        rl = 27;
        break;
#ifdef ESW_TEST_HOOKS
    case 0x7E:   /* TEST_PANIC: verify that a crash leaves the switches open and the board updatable */
        g_test_panic = 1;
        break;
#endif
    default:
        st = ESW_ERR_UNSUPPORTED;
        break;
    }
done:
    if (s && q[1] != ESW_CMD_SWD_STATS)
        s->last_op_us = now_us() - t0;
    r[2] = (uint8_t)(int8_t)st;
    return rl;
}

static void proto_task(void *arg)
{
    (void)arg;
    /* dedicated-GPIO engines belong to this core (core 1): create them here, with every switch open */
    for (int c = 0; c < BOARD_NUM_CHANNELS; c++)
        g_dg_ok[c] = esw_dgpio_create(&g_dg[c], board_ch[c].swclk, board_ch[c].swdio) == ESP_OK;
    esw_frame_rx_init(&g_rx);
    for (;;) {
        int n = usb_serial_jtag_read_bytes(g_usb, sizeof(g_usb), pdMS_TO_TICKS(20));
        supervise();
        for (int i = 0; i < n; i++) {
            int pl = esw_frame_rx_byte(&g_rx, g_usb[i], g_req, sizeof(g_req));
            if (pl < 2)
                continue;
            g_last_rx_ms = now_ms();
            g_busy = 1;
            int rl = handle(g_req, pl, g_rsp);
            g_busy = 0;
            g_last_rx_ms = now_ms();
            size_t fl = esw_frame_encode(g_rsp, (size_t)rl, g_frame, sizeof(g_frame));
            usb_serial_jtag_write_bytes(g_frame, fl, pdMS_TO_TICKS(500));
            if (g_test_panic) {
                vTaskDelay(pdMS_TO_TICKS(50));
                abort();   /* deliberate panic, switches possibly closed: __wrap_esp_panic_handler must open them */
            }
            if (g_reboot != 0xFF) {
                vTaskDelay(pdMS_TO_TICKS(50));   /* let the response leave */
                for (int c = 0; c < BOARD_NUM_CHANNELS; c++)
                    detach(c);
                /* Stop the interlock for good: keep g_lock until the restart, so the tick task (which skips its
                 * body without the lock) can never re-close a switch between here and the ROM. */
                if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(200))) {
                    for (int c = 0; c < BOARD_NUM_CHANNELS; c++)
                        esw_channel_isolate(&g_ch[c]);
                }
                board_isolate_now();
                board_safe_init();
                if (g_reboot == 1)
                    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_restart();
            }
        }
    }
}

/* Panic path: open every switch before the panic handler prints, waits or halts. A panic resets only the CPUs, so
 * the GPIO latches would otherwise keep the enables high until app_main (the bootloader hook covers the reset). */
void __real_esp_panic_handler(void *info);
void IRAM_ATTR __wrap_esp_panic_handler(void *info)
{
    board_isolate_now();
    __real_esp_panic_handler(info);
}

void app_main(void)
{
    board_safe_init();   /* before anything else: every switch open, every driver idle */
    esp_register_shutdown_handler(board_isolate_now);   /* every esp_restart(), e.g. a future OTA reboot */
    /* Brown-out: the BOD ISR (core 0, same core as the interlock tick) calls this before anything else, so the
     * switches open while the tool rail sags instead of after the reset. */
    if (esp_brownout_register_callback(board_isolate_now) != ESP_OK)
        ESP_LOGE("esw", "brown-out isolation callback not registered");
    board_adc_init();
    g_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < BOARD_NUM_CHANNELS; i++)
        esw_channel_init(&g_ch[i], &board_channel_ops, (void *)&board_ch[i], &board_channel_cfg);
    usb_serial_jtag_driver_config_t uc = { .rx_buffer_size = 8192, .tx_buffer_size = 4096 };   /* 3 frames in flight */
    usb_serial_jtag_driver_install(&uc);
    xTaskCreatePinnedToCore(tick_task, "esw_tick", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(proto_task, "esw_proto", 8192, NULL, 5, NULL, 1);
    /* Rollback: an OTA-installed image stays "pending" until it confirms it came up (USB link and tasks
     * running). An image that crashes before this point is rolled back by the bootloader on the next reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_ota_mark_app_valid_cancel_rollback();
}
