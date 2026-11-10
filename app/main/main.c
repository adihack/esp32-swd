/* SPDX-License-Identifier: MIT */
/* esp-swd test firmware for r5 (milestone M1 preview).
 *
 * Link: the ESP32-S3 USB-Serial-JTAG CDC port (no TinyUSB yet; esptool keeps working without a BOOT button),
 * framed with lib/proto (COBS + CRC-16). Tasks:
 *   esw_tick  (core 0, every 2 ms): isolation interlock for both channels, host-presence lease, status LED
 *   esw_uart  (core 0, every 1 ms): DUT UART RX -> USB events, pin routing after each switch closing; the UART
 *                                   interrupts live on core 0 too, away from the SWD engine's masked packets
 *   esw_proto (core 1):             dispatcher: reads USB frames, answers device/state/UART commands itself and
 *                                   queues each channel's commands to that channel's worker
 *   esw_swd_a (core 1), esw_swd_b (core 0): one worker per channel, so both channels flash at the same time. Each
 *                                   owns its channel's SWD engine (dedicated GPIO is per core), session and
 *                                   supervision; commands of one channel run in order, the two channels in parallel
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
#include "esp_partition.h"
#include "esw/proto.h"
#include "esw/channel.h"
#include "esw/adiv5.h"
#include "esw/swd_bitbang.h"
#include "esw/target.h"
#include "esw_port.h"
#include "esw_dgpio.h"
#include "esw_uart.h"
#include "board_r5.h"

#define FW_NAME "esp-swd 0.4.0 r5/usj"
#define HOST_LEASE_MS 3000u      /* no valid frame for this long = host gone (then the channel grace period runs) */
#define MAX_WORDS 512u
#define UART_CHUNK 2048u          /* RX bytes per event frame */
#define UART_NOT_ROUTED 0xFFFFFFFFu

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

/* DUT UART bridge, one per channel. Everything that touches the UART hardware or its pads (back-end start/stop,
 * routing, RX streaming) runs on the esw_uart task, core 0; the proto task asks for open/close through g_ureq. */
typedef struct {
    esw_uart_t u;
    volatile uint8_t open;    /* stream open (UART_OPEN .. UART_CLOSE or host loss) */
    volatile uint8_t routed;  /* the pads are on the UART for the current closing of the group */
    uint8_t pend;             /* ESW_UART_RX_* flags for the next RX event */
    uint32_t baud, flags;
    uint32_t gen;             /* channel.opened[UART] when the pads were routed to the UART; UART_NOT_ROUTED */
    uint32_t rx, tx, usb_drop, route_fail;
} uart_ch_t;

typedef struct {
    uint8_t op, ch, be;
    uint32_t baud, flags, actual;
    esp_err_t result;
} uart_req_t;
enum { UREQ_OPEN = 1, UREQ_STOP = 2 };
static QueueHandle_t g_ureq;          /* proto -> uart task: uart_req_t * */
static SemaphoreHandle_t g_udone;     /* uart task -> proto: request done */
static int g_uhci_owner = -1;         /* channel on the DMA back-end (the ESP32-S3 has one UHCI); uart task only */

static esw_channel_t g_ch[BOARD_NUM_CHANNELS];
static uart_ch_t g_u[BOARD_NUM_CHANNELS];
static volatile uint8_t g_uart_ready;
static esw_dgpio_t g_dg[BOARD_NUM_CHANNELS];   /* created at boot by each channel's worker on its core */
static uint8_t g_dg_ok[BOARD_NUM_CHANNELS];
static SemaphoreHandle_t g_lock;
static session_t g_s[BOARD_NUM_CHANNELS];
static volatile uint32_t g_last_rx_ms, g_busy;   /* g_busy: commands in progress (atomic counter) */
static esw_frame_rx_t g_rx;
static uint8_t g_req[ESW_PROTO_MAX_PAYLOAD], g_rsp[ESW_PROTO_MAX_PAYLOAD], g_usb[4096];   /* dispatcher only */
static uint32_t g_words[BOARD_NUM_CHANNELS][MAX_WORDS];

/* Per-channel worker: a pool of request buffers (the host keeps at most ~7 KB of requests unanswered, so a pool
 * never runs dry in normal use), its own response and frame buffers. */
#define JOB_POOL ESW_PROTO_CH_QUEUE
#define JOB_DETACH 0xFFFFu   /* job length marking "end the session" (reboot path) */
typedef struct {
    uint16_t len;
    uint8_t buf[ESW_PROTO_MAX_PAYLOAD];
} job_t;
typedef struct {
    QueueHandle_t todo, free;   /* job_t * */
    SemaphoreHandle_t done;     /* JOB_DETACH finished; also "engine created" at start */
    job_t jobs[JOB_POOL];
    uint8_t rsp[ESW_PROTO_MAX_PAYLOAD], frame[ESW_FRAME_MAX];
    uint32_t last_yield_ms;
} worker_t;
static worker_t g_w[BOARD_NUM_CHANNELS];

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
static void uart_close(int ch);
static void supervise_uart(void)
{
    for (int ch = 0; ch < BOARD_NUM_CHANNELS; ch++)
        if (g_u[ch].open && host_lost(ch))
            uart_close(ch);   /* the channel already released the hold; stop streaming to nobody */
}

/* Called by the channel's worker between commands (<= 20 ms): the session's SWD traffic must run on its core. */
static void supervise_session(int ch)
{
    if (!g_s[ch].attached)
        return;
    if (!session_wire_ok(ch))
        drop_session(ch);
    else if (host_lost(ch))
        detach(ch);   /* clean end while the switch is still closed; then the group opens */
}

static esw_err_t attach(int ch, uint32_t clock, uint32_t phy_kind, uint32_t core)
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
        e = esw_target_detect_core(&s->tgt, &s->dap, &esw_port_clock, core);
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

/* ---- DUT UART bridge ------------------------------------------------------------------------------------ */
/* The pads belong to the UART only after the group closed: the board's prepare step (run by the interlock right
 * before every closing) puts TX on a plain GPIO at idle high. Route once per closing, latched by the opened[]
 * counter, under the channel lock so a closing cannot slip in between. Uart task only. */
static void uart_sync_route(int c)
{
    uart_ch_t *u = &g_u[c];
    if (!xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)))
        return;
    uint32_t closed = esw_channel_state(&g_ch[c], ESW_GROUP_UART) == ESW_GSTATE_CLOSED;
    uint32_t gen = g_ch[c].opened[ESW_GROUP_UART];
    if (closed && u->open && u->gen != gen) {
        if (esw_uart_route(&u->u) == ESP_OK)
            u->gen = gen;
        else
            u->route_fail++;
    }
    u->routed = u->open && closed && u->gen == gen;
    xSemaphoreGive(g_lock);
}

/* Uart task only: TX back to a plain GPIO at idle high before the back-end changes (a stop gates the UART clock). */
static void uart_unroute(int c)
{
    uart_ch_t *u = &g_u[c];
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        esw_uart_unroute(&u->u);
        u->gen = UART_NOT_ROUTED;
        u->routed = 0;
        xSemaphoreGive(g_lock);
    }
}

static void uart_do(uart_req_t *q)
{
    uart_ch_t *u = &g_u[q->ch];
    u->open = 0;
    uart_unroute(q->ch);
    if (g_uhci_owner == q->ch)
        g_uhci_owner = -1;   /* esw_uart_start/stop releases it first */
    if (q->op == UREQ_STOP) {
        esw_uart_stop(&u->u);
        q->result = ESP_OK;
        return;
    }
    /* the DMA back-end goes to the first channel that opens while the UHCI is free; ESW_UART_NO_DMA forces IRQ */
    esw_uart_be_t be = (!(q->flags & ESW_UART_NO_DMA) && g_uhci_owner < 0) ? ESW_UART_BE_DMA : ESW_UART_BE_IRQ;
    q->result = esw_uart_start(&u->u, q->baud, q->flags, be, &q->actual);
    if (q->result != ESP_OK && be == ESW_UART_BE_DMA) {
        be = ESW_UART_BE_IRQ;   /* no DMA memory or channel: the hardened interrupt path still works */
        q->result = esw_uart_start(&u->u, q->baud, q->flags, be, &q->actual);
    }
    if (q->result != ESP_OK) {
        esw_uart_stop(&u->u);
        return;
    }
    if (be == ESW_UART_BE_DMA)
        g_uhci_owner = q->ch;
    q->be = (uint8_t)be;
    u->baud = q->actual;
    u->flags = q->flags;
    u->pend = 0;
    u->rx = u->tx = u->usb_drop = u->route_fail = 0;
    u->open = 1;
    uart_sync_route(q->ch);   /* group already closed (e.g. a baud change): route now */
}

/* Proto task: run a request on the uart task (core 0) and wait for it. */
static esp_err_t uart_request(uart_req_t *q)
{
    if (!g_uart_ready)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_udone, 0);
    if (xQueueSend(g_ureq, &q, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(g_udone, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return q->result;
}

static void uart_hold(int ch, uint32_t on)
{
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100))) {
        if (on)
            (void)esw_channel_hold(&g_ch[ch], ESW_GROUP_UART, ESW_HOLD_HOST_UART);
        else
            (void)esw_channel_release(&g_ch[ch], ESW_GROUP_UART, ESW_HOLD_HOST_UART);
        xSemaphoreGive(g_lock);
    }
}

/* Proto task: drain TX (never open the switch in the middle of a byte), release the hold, stop the back-end (this
 * frees the UHCI for the other channel). */
static void uart_close(int ch)
{
    uart_ch_t *u = &g_u[ch];
    if (!u->open)
        return;
    for (int i = 0; i < 200 && !esw_uart_tx_idle(&u->u); i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    u->open = 0;
    uart_hold(ch, 0);
    uart_req_t q = { .op = UREQ_STOP, .ch = (uint8_t)ch };
    (void)uart_request(&q);
}

/* One encoded RX event per channel waits here until USB takes it: a host that stalls (GC pause, busy thread) makes
 * the back-end's 32 KB ring absorb the backlog instead of losing data. A frame is dropped (counted, flagged in the
 * next event) only after USB refused it for a whole second, i.e. nobody reads. usb_serial_jtag_is_connected() is
 * not usable for this: its SOF monitor reports short false disconnects under load (measured: 120 bytes lost). */
#define UART_USB_GIVE_UP_MS 1000u
static uint8_t g_uev[BOARD_NUM_CHANNELS][ESW_COBS_MAX(9 + UART_CHUNK + 2) + 1];
static size_t g_uev_len[BOARD_NUM_CHANNELS];
static uint32_t g_uev_bytes[BOARD_NUM_CHANNELS], g_uev_t0[BOARD_NUM_CHANNELS];

static uint32_t uart_flush_event(int c)
{
    uart_ch_t *u = &g_u[c];
    if (!g_uev_len[c])
        return 1;
    /* one call = one whole frame (the driver queues all of it or nothing), so frames from this task and proto
     * responses never interleave */
    if (usb_serial_jtag_write_bytes(g_uev[c], g_uev_len[c], pdMS_TO_TICKS(10)) == (int)g_uev_len[c]) {
        u->rx += g_uev_bytes[c];
        g_uev_len[c] = 0;
        g_uev_t0[c] = 0;
        return 1;
    }
    uint32_t t = now_ms() | 1u;
    if (!g_uev_t0[c]) {
        g_uev_t0[c] = t;
    } else if (t - g_uev_t0[c] > UART_USB_GIVE_UP_MS) {
        u->usb_drop += g_uev_bytes[c];
        u->pend |= ESW_UART_RX_LOST;
        g_uev_len[c] = 0;
        g_uev_t0[c] = 0;
        return 1;
    }
    return 0;
}

static void uart_task(void *arg)
{
    (void)arg;
    static uint8_t pl[9 + UART_CHUNK];
    for (int c = 0; c < BOARD_NUM_CHANNELS; c++) {
        g_u[c].gen = UART_NOT_ROUTED;
        (void)esw_uart_init(&g_u[c].u, c == 0 ? UART_NUM_1 : UART_NUM_2, board_ch[c].uart_tx, board_ch[c].uart_rx);
    }
    g_uart_ready = 1;
    for (;;) {
        uart_req_t *q;
        while (xQueueReceive(g_ureq, &q, 0) == pdTRUE) {
            uart_do(q);
            xSemaphoreGive(g_udone);
        }
        for (int c = 0; c < BOARD_NUM_CHANNELS; c++) {
            uart_ch_t *u = &g_u[c];
            if (u->u.be == ESW_UART_BE_OFF)
                continue;
            u->pend |= (uint8_t)esw_uart_poll(&u->u);
            if (!uart_flush_event(c))
                continue;   /* USB still busy: the back-end's ring keeps filling meanwhile */
            if (!u->open)
                continue;
            uart_sync_route(c);
            if (!u->routed)
                continue;
            for (;;) {
                size_t got = esw_uart_read(&u->u, pl + 9, UART_CHUNK);
                if (!got)
                    break;
                pl[0] = 0;
                pl[1] = ESW_EVT_UART_RX;
                pl[2] = 0;
                pl[3] = (uint8_t)c;
                pl[4] = u->pend;
                u->pend = 0;
                wr32(pl + 5, now_us());
                g_uev_len[c] = esw_frame_encode(pl, 9u + got, g_uev[c], sizeof(g_uev[c]));
                g_uev_bytes[c] = (uint32_t)got;
                if (!uart_flush_event(c))
                    break;
            }
        }
        vTaskDelay(1);
    }
}

/* ---- command dispatcher ------------------------------------------------------------------------------- */
static uint8_t g_reboot = 0xFF;   /* 0xFF: no reboot requested; 0 normal, 1 ROM download */
static uint8_t g_test_panic;

#ifdef ESW_TEST_HOOKS
/* TEST_FLASH_STRESS: erase + write a 4 KB sector of the unused "images" partition every gap ms, in the
 * background. Every erase turns the flash cache off for about 40 ms: only IRAM interrupt handlers run then, tasks
 * and USB stall. The worst case for UART reception. */
static volatile uint32_t g_fstress_ops, g_fstress_busy, g_fstress_gap;
static void flash_stress_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg, t0 = now_ms(), n = 0;
    static uint8_t buf[4096];
    memset(buf, 0xA5, sizeof(buf));
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "images");
    while (p && now_ms() - t0 < ms) {
        uint32_t off = (n % (p->size / 4096u)) * 4096u;
        if (esp_partition_erase_range(p, off, 4096) != ESP_OK || esp_partition_write(p, off, buf, sizeof(buf)) != ESP_OK)
            break;
        g_fstress_ops = ++n;
        if (g_fstress_gap)
            vTaskDelay(pdMS_TO_TICKS(g_fstress_gap));
    }
    g_fstress_busy = 0;
    vTaskDelete(NULL);
}
#endif

/* SWD_ATTACH / TGT_UNLOCK response body: dpidr, locked, nregions, khz, part, regions. Returns the response length. */
static int target_info(const session_t *s, uint8_t *r)
{
    const esw_target_t *t = &s->tgt;
    wr32(r + 3, s->dap.dpidr);
    r[7] = (uint8_t)t->locked;
    r[8] = (uint8_t)t->nregions;
    r[9] = (uint8_t)((s->hz / 1000u) & 0xFFu);   /* actual SWCLK in kHz (dgpio), 0 for bit-bang */
    r[10] = (uint8_t)((s->hz / 1000u) >> 8);
    memset(r + 11, 0, 32);
    if (t->drv)
        memcpy(r + 11, t->part, strnlen(t->part, 31));   /* 32-byte field, always NUL-terminated */
    int rl = 43;
    for (uint32_t i = 0; i < t->nregions; i++, rl += 16) {
        wr32(r + rl, t->regions[i].start);
        wr32(r + rl + 4, t->regions[i].size);
        wr32(r + rl + 8, t->regions[i].sector_size);
        wr32(r + rl + 12, (uint32_t)t->regions[i].kind);
    }
    return rl;
}

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
        st = attach(ch, rd32(a + 1), an >= 6 ? a[5] : 0u, an >= 7 ? a[6] : 0u);   /* optional phy, core */
        rl = target_info(s, r);
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
        rl = target_info(s, r);   /* the memory map now: a re-attach would reset, and an nRF5340 shuts again */
        break;
    case ESW_CMD_TGT_RESET:
        CHAN();
        ATTACHED();
        NEED(5);
        st = esw_target_finish(&s->tgt);   /* a resident loader ends cleanly before the reset kills it */
        if (st == ESW_OK)
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
        st = esw_target_sync(&s->tgt);   /* a streaming loader's handed-over data lands first */
        if (st == ESW_OK)
            st = esw_mem_read_block(&s->tgt.mem, rd32(a + 1), g_words[ch], nw);
        if (st == ESW_OK) {
            memcpy(r + rl, g_words[ch], nw * 4u);
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
        memcpy(g_words[ch], a + 5, nw * 4u);
        if (q[1] == ESW_CMD_MEM_WRITE)
            st = esw_target_sync(&s->tgt) == ESW_OK ? esw_mem_write_block(&s->tgt.mem, addr, g_words[ch], nw)
                                                    : ESW_ERR_LOADER;
        else if (q[1] == ESW_CMD_TGT_PROGRAM)
            st = esw_target_program(&s->tgt, addr, g_words[ch], nw);
        else
            st = esw_target_verify(&s->tgt, addr, g_words[ch], nw);
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
                if ((i & 0xFFFFu) == 0xFFFFu)
                    vTaskDelay(1);   /* idle task of this core: the task watchdog watches core 0's */
                st = esw_dp_read(&s->dap, ESW_DP_DPIDR, &v);
                errors += (st == ESW_OK && v != s->dap.dpidr);
            }
        } else if (mode == 1 || mode == 2) {
            for (uint32_t done = 0; done < cnt && st == ESW_OK; done += MAX_WORDS) {
                uint32_t n = cnt - done < MAX_WORDS ? cnt - done : MAX_WORDS;
                uint32_t addr = base + ((done * 4u) % 0x20000u);
                if (mode == 1) {
                    for (uint32_t i = 0; i < n; i++)
                        g_words[ch][i] = (addr + 4u * i) ^ 0xA5C3E1F0u;
                    st = esw_mem_write_block(&s->tgt.mem, addr, g_words[ch], n);
                } else {
                    st = esw_mem_read_block(&s->tgt.mem, addr, g_words[ch], n);
                    for (uint32_t i = 0; i < n && st == ESW_OK; i++)
                        errors += g_words[ch][i] != ((addr + 4u * i) ^ 0xA5C3E1F0u);
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
    case ESW_CMD_DAP_RAW: {
        CHAN();
        ATTACHED();
        NEED(8);
        uint32_t op = a[1], ap = a[2], reg = a[3], v = rd32(a + 4);
        if (op > 3 || (reg & 0x03u)) {
            st = ESW_ERR_ARG;
            break;
        }
        if (op < 2) {   /* DP: bank select around the access */
            uint32_t bank = reg >> 4;
            if (bank)
                st = esw_dp_write(&s->dap, ESW_DP_SELECT, bank);
            if (st == ESW_OK)
                st = op == 0 ? esw_dp_read(&s->dap, (uint8_t)(reg & 0x0Cu), &v)
                             : esw_dp_write(&s->dap, (uint8_t)(reg & 0x0Cu), v);
            if (bank)
                (void)esw_dp_write(&s->dap, ESW_DP_SELECT, 0);
        } else {
            st = op == 2 ? esw_ap_read(&s->dap, (uint8_t)ap, (uint8_t)reg, &v)
                         : esw_ap_write(&s->dap, (uint8_t)ap, (uint8_t)reg, v);
            esw_memap_invalidate(&s->tgt.mem);   /* CSW/TAR of the session's MEM-AP may have changed */
        }
        wr32(r + 3, v);
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
    case ESW_CMD_UART_OPEN: {
        CHAN();
        NEED(9);
        uart_ch_t *u = &g_u[ch];
        uart_req_t q = { .op = UREQ_OPEN, .ch = (uint8_t)ch, .baud = rd32(a + 1), .flags = rd32(a + 5) };
        u->open = 0;   /* no RX events while the settings change */
        esp_err_t e = uart_request(&q);
        if (e != ESP_OK) {
            st = e == ESP_ERR_INVALID_ARG ? ESW_ERR_ARG : ESW_ERR_STATE;
            uart_hold(ch, 0);
            break;
        }
        uart_hold(ch, 1);
        wr32(r + 3, q.actual);
        r[7] = q.be;   /* 1 interrupt-driven, 2 DMA */
        rl = 8;
        break;
    }
    case ESW_CMD_UART_CLOSE:
        CHAN();
        uart_close(ch);
        break;
    case ESW_CMD_UART_WRITE: {
        CHAN();
        uart_ch_t *u = &g_u[ch];
        size_t n = 0;
        r[3] = r[4] = 0;
        rl = 5;
        if (!u->open) {
            st = ESW_ERR_STATE;
            break;
        }
        if (!u->routed) {
            /* closed but the uart task has not routed the pads yet (within 1 ms): accept nothing, the host retries;
             * isolated or VTref missing: nothing may be driven towards the DUT */
            if (group_state(ch, ESW_GROUP_UART) != ESW_GSTATE_CLOSED)
                st = ESW_ERR_INTERLOCK;
            break;
        }
        n = esw_uart_write(&u->u, a + 1, (size_t)(an - 1));
        u->tx += (uint32_t)n;
        r[3] = (uint8_t)n;
        r[4] = (uint8_t)(n >> 8);
        break;
    }
    case ESW_CMD_UART_STATS: {
        CHAN();
        uart_ch_t *u = &g_u[ch];
        esw_uart_t *x = &u->u;
        r[3] = u->open;
        r[4] = u->routed;
        r[5] = (uint8_t)x->be;
        r[6] = 0;
        const uint32_t v[] = { u->baud, u->rx, u->tx, u->usb_drop, x->fifo_ovf, x->buf_full, x->frame_err,
                               x->parity_err, x->brk, x->dma_err };
        for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++)
            wr32(r + 7 + 4 * i, v[i]);
        rl = 7 + 4 * (int)(sizeof(v) / sizeof(v[0]));
        break;
    }
#ifdef ESW_TEST_HOOKS
    case 0x7D:   /* TEST_FLASH_STRESS: ms u32 (0: just report), gap_ms u32 -> erase+write cycles, busy u8 */
        NEED(4);
        if (rd32(a) && !g_fstress_busy) {
            g_fstress_gap = an >= 8 ? rd32(a + 4) : 0;
            g_fstress_busy = 1;
            g_fstress_ops = 0;
            if (xTaskCreatePinnedToCore(flash_stress_task, "esw_fstress", 3072, (void *)(uintptr_t)rd32(a), 2, NULL, 1) !=
                pdPASS)
                g_fstress_busy = 0;
        }
        wr32(r + 3, g_fstress_ops);
        r[7] = (uint8_t)g_fstress_busy;
        rl = 8;
        break;
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

/* Channel commands go to the channel's worker: hold/release (a release may end the session) and all SWD/target
 * commands. Everything else (device, state, UART) is answered by the dispatcher. */
static int channel_of(const uint8_t *q, int n)
{
    uint8_t c = q[1];
    if (n < 3 || q[2] >= BOARD_NUM_CHANNELS)
        return -1;
    if (c == ESW_CMD_CH_HOLD || c == ESW_CMD_CH_RELEASE || (c >= 0x20 && c <= 0x2F))
        return q[2];
    return -1;
}

static void send_frame(const uint8_t *rsp, int rl, uint8_t *frame)
{
    size_t fl = esw_frame_encode(rsp, (size_t)rl, frame, ESW_FRAME_MAX);
    usb_serial_jtag_write_bytes(frame, fl, pdMS_TO_TICKS(500));   /* one frame per call: never interleaved */
}

static void swd_worker(void *arg)
{
    const int ch = (int)(uintptr_t)arg;
    worker_t *w = &g_w[ch];
    /* dedicated-GPIO engine of this channel belongs to this core: create it here, with every switch open */
    g_dg_ok[ch] = esw_dgpio_create(&g_dg[ch], board_ch[ch].swclk, board_ch[ch].swdio) == ESP_OK;
    xSemaphoreGive(w->done);
    w->last_yield_ms = now_ms();
    for (;;) {
        job_t *j = NULL;
        if (xQueueReceive(w->todo, &j, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (j->len == JOB_DETACH) {
                detach(ch);
                xQueueSend(w->free, &j, 0);
                xSemaphoreGive(w->done);
                continue;
            }
            __atomic_fetch_add(&g_busy, 1, __ATOMIC_SEQ_CST);
            int rl = handle(j->buf, j->len, w->rsp);
            g_last_rx_ms = now_ms();
            __atomic_fetch_sub(&g_busy, 1, __ATOMIC_SEQ_CST);
            xQueueSend(w->free, &j, 0);
            send_frame(w->rsp, rl, w->frame);
            /* back-to-back commands never block on the queue: give the idle task of this core a tick now and
             * then (the task watchdog watches core 0's) */
            if (now_ms() - w->last_yield_ms >= 100u) {
                vTaskDelay(1);
                w->last_yield_ms = now_ms();
            }
        } else {
            w->last_yield_ms = now_ms();
        }
        supervise_session(ch);
    }
}

static void proto_task(void *arg)
{
    (void)arg;
    static uint8_t frame[ESW_FRAME_MAX];
    /* workers: channel A next to this task on core 1 (below it, so frames are still read while A works), channel
     * B on core 0 with the tick and UART tasks (above it). One after the other: each creates its SWD engine. */
    static const struct { const char *name; int core, prio; } wcfg[2] = { { "esw_swd_a", 1, 4 }, { "esw_swd_b", 0, 5 } };
    for (int c = 0; c < BOARD_NUM_CHANNELS; c++) {
        worker_t *w = &g_w[c];
        w->todo = xQueueCreate(JOB_POOL, sizeof(job_t *));
        w->free = xQueueCreate(JOB_POOL, sizeof(job_t *));
        w->done = xSemaphoreCreateBinary();
        for (uint32_t i = 0; i < JOB_POOL; i++) {
            job_t *j = &w->jobs[i];
            xQueueSend(w->free, &j, 0);
        }
        xTaskCreatePinnedToCore(swd_worker, wcfg[c].name, 8192, (void *)(uintptr_t)c, wcfg[c].prio, NULL,
                                wcfg[c].core);
        xSemaphoreTake(w->done, pdMS_TO_TICKS(1000));
    }
    esw_frame_rx_init(&g_rx);
    for (;;) {
        int n = usb_serial_jtag_read_bytes(g_usb, sizeof(g_usb), pdMS_TO_TICKS(20));
        supervise_uart();
        for (int i = 0; i < n; i++) {
            int pl = esw_frame_rx_byte(&g_rx, g_usb[i], g_req, sizeof(g_req));
            if (pl < 2)
                continue;
            g_last_rx_ms = now_ms();
            int ch = channel_of(g_req, pl);
            if (ch >= 0) {
                job_t *j = NULL;
                if (xQueueReceive(g_w[ch].free, &j, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    j->len = (uint16_t)pl;
                    memcpy(j->buf, g_req, (size_t)pl);
                    xQueueSend(g_w[ch].todo, &j, portMAX_DELAY);   /* cannot block: as many slots as buffers */
                } else {   /* the host broke its budget, or the worker is stuck: answer instead of dropping */
                    g_rsp[0] = g_req[0];
                    g_rsp[1] = (uint8_t)(g_req[1] | 0x80u);
                    g_rsp[2] = (uint8_t)(int8_t)ESW_ERR_TIMEOUT;
                    send_frame(g_rsp, 3, frame);
                }
                continue;
            }
            __atomic_fetch_add(&g_busy, 1, __ATOMIC_SEQ_CST);
            int rl = handle(g_req, pl, g_rsp);
            __atomic_fetch_sub(&g_busy, 1, __ATOMIC_SEQ_CST);
            g_last_rx_ms = now_ms();
            send_frame(g_rsp, rl, frame);
            if (g_test_panic) {
                vTaskDelay(pdMS_TO_TICKS(50));
                abort();   /* deliberate panic, switches possibly closed: __wrap_esp_panic_handler must open them */
            }
            if (g_reboot != 0xFF) {
                vTaskDelay(pdMS_TO_TICKS(50));   /* let the response leave */
                for (int c = 0; c < BOARD_NUM_CHANNELS; c++) {   /* each worker ends its session on its core */
                    job_t *j = NULL;
                    if (xQueueReceive(g_w[c].free, &j, pdMS_TO_TICKS(500)) == pdTRUE) {
                        j->len = JOB_DETACH;
                        xQueueSend(g_w[c].todo, &j, portMAX_DELAY);
                        xSemaphoreTake(g_w[c].done, pdMS_TO_TICKS(2000));
                    }
                    uart_close(c);
                }
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
        /* Mid-frame: let the rest of it arrive (USB delivers 64-byte packets) instead of waking per packet, which
         * cost the SWD worker on this core two context switches each time. A complete frame is never delayed. */
        if (n > 0 && g_rx.len > 0)
            vTaskDelay(1);
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
    g_ureq = xQueueCreate(2, sizeof(uart_req_t *));
    g_udone = xSemaphoreCreateBinary();
    for (int i = 0; i < BOARD_NUM_CHANNELS; i++)
        esw_channel_init(&g_ch[i], &board_channel_ops, (void *)&board_ch[i], &board_channel_cfg);
    /* RX 8 KB: 3 request frames in flight; TX 16 KB: responses plus UART RX events while the host is briefly slow */
    usb_serial_jtag_driver_config_t uc = { .rx_buffer_size = 8192, .tx_buffer_size = 16384 };
    usb_serial_jtag_driver_install(&uc);
    xTaskCreatePinnedToCore(tick_task, "esw_tick", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(uart_task, "esw_uart", 4096, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(proto_task, "esw_proto", 8192, NULL, 5, NULL, 1);
    /* Rollback: an OTA-installed image stays "pending" until it confirms it came up (USB link and tasks
     * running). An image that crashes before this point is rolled back by the bootloader on the next reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_ota_mark_app_valid_cancel_rollback();
}
