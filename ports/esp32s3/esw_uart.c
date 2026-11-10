/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "esw_uart.h"
#include "esw/proto.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"
#include "hal/uart_periph.h"
#include "hal/uhci_ll.h"
#include "hal/uhci_types.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"

#define RING_SZ 32768u        /* RX ring, both back-ends */
#define TXRING_SZ 8192u       /* TX ring, both back-ends */
#define RXD_N 32u             /* DMA: hardware ring of 32 x 256 B = 8 KB */
#define RXD_SZ 256u
#define TXD_N 3u              /* one TX chunk: up to TXRING_SZ contiguous bytes, at most 4092 per descriptor */
#define TXD_MAX 4092u
#define IRQ_RX_FULL 32        /* IRQ: FIFO-full interrupt at 32 of 128 bytes */
#define IRQ_RX_TOUT 3         /* IRQ: deliver after 3 idle character times */
#define LINE_ERRS (UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR | UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET)

/* ---- DMA back-end: interrupts ------------------------------------------------------------------------------ */
static void IRAM_ATTR rx_drain(esw_uart_t *u)
{
    for (uint32_t k = 0; k < RXD_N; k++) {
        volatile dma_descriptor_t *d = &u->rxd[u->rx_next];
        if (d->dw0.owner != DMA_DESCRIPTOR_BUFFER_OWNER_CPU)
            break;
        uint32_t n = d->dw0.length, space = RING_SZ - (u->rh - u->rt);
        const uint8_t *src = (const uint8_t *)d->buffer;
        if (n > space) {
            u->buf_full++;
            u->pend |= ESW_UART_RX_LOST;
            n = space;
        }
        uint32_t off = u->rh & (RING_SZ - 1u), first = n < RING_SZ - off ? n : RING_SZ - off;
        memcpy(u->ring + off, src, first);
        memcpy(u->ring, src + first, n - first);
        u->rh += n;
        d->dw0.length = 0;
        d->dw0.suc_eof = 0;
        d->dw0.err_eof = 0;
        d->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;   /* hand back last */
        u->rx_next = (u->rx_next + 1u) % RXD_N;
    }
}

static bool IRAM_ATTR rx_isr(gdma_channel_handle_t ch, gdma_event_data_t *ev, void *arg)
{
    (void)ch;
    (void)ev;
    rx_drain((esw_uart_t *)arg);
    return false;
}

/* The DMA reached a descriptor the CPU still owns (cannot happen while this ISR keeps up): drain, restart. */
static bool IRAM_ATTR rx_err_isr(gdma_channel_handle_t ch, gdma_event_data_t *ev, void *arg)
{
    (void)ev;
    esw_uart_t *u = arg;
    u->dma_err++;
    u->pend |= ESW_UART_RX_LOST;
    rx_drain(u);
    gdma_stop(ch);
    gdma_reset(ch);
    gdma_start(ch, (intptr_t)&u->rxd[u->rx_next]);
    return false;
}

/* Start the next TX chunk if there is data and nothing in flight. Caller holds u->lock. */
static void IRAM_ATTR tx_kick_locked(esw_uart_t *u)
{
    if (u->tx_busy || u->th == u->tt)
        return;
    uint32_t off = u->tt & (TXRING_SZ - 1u), len = u->th - u->tt;
    if (len > TXRING_SZ - off)
        len = TXRING_SZ - off;
    uint32_t p = off, left = len, k = 0;
    while (left) {
        uint32_t c = left > TXD_MAX ? TXD_MAX : left;
        dma_descriptor_t *d = &u->txd[k];
        d->dw0.size = (c + 3u) & ~3u;
        d->dw0.length = c;
        d->dw0.err_eof = 0;
        d->dw0.suc_eof = left == c;
        d->buffer = u->txring + p;
        d->next = left == c ? NULL : &u->txd[k + 1u];
        d->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        p += c;
        left -= c;
        k++;
    }
    u->tx_len = len;
    u->tx_busy = 1;
    gdma_start(u->txch, (intptr_t)&u->txd[0]);
}

static bool IRAM_ATTR tx_isr(gdma_channel_handle_t ch, gdma_event_data_t *ev, void *arg)
{
    (void)ch;
    (void)ev;
    esw_uart_t *u = arg;
    portENTER_CRITICAL_ISR(&u->lock);
    u->tt += u->tx_len;
    u->tx_len = 0;
    u->tx_busy = 0;
    tx_kick_locked(u);
    portEXIT_CRITICAL_ISR(&u->lock);
    return false;
}

/* ---- DMA back-end: setup ----------------------------------------------------------------------------------- */
static void dma_free(esw_uart_t *u)
{
    if (u->rxch) {
        gdma_stop(u->rxch);
        gdma_disconnect(u->rxch);
        gdma_del_channel(u->rxch);
    }
    if (u->txch) {
        gdma_stop(u->txch);
        gdma_disconnect(u->txch);
        gdma_del_channel(u->txch);
    }
    u->rxch = u->txch = NULL;
    heap_caps_free(u->rxd);
    heap_caps_free(u->txd);
    heap_caps_free(u->rxbuf);
    heap_caps_free(u->ring);
    heap_caps_free(u->txring);
    u->rxd = u->txd = NULL;
    u->rxbuf = u->ring = u->txring = NULL;
    uhci_dev_t *hw = UHCI_LL_GET_HW(0);
    uhci_ll_attach_uart_port(hw, -1);
    PERIPH_RCC_ATOMIC() {
        uhci_ll_enable_bus_clock(0, false);
    }
}

static esp_err_t dma_start(esw_uart_t *u)
{
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    u->rxd = heap_caps_aligned_calloc(4, RXD_N, sizeof(dma_descriptor_t), caps);
    u->txd = heap_caps_aligned_calloc(4, TXD_N, sizeof(dma_descriptor_t), caps);
    u->rxbuf = heap_caps_aligned_calloc(4, RXD_N, RXD_SZ, caps);
    u->ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_INTERNAL);
    u->txring = heap_caps_aligned_calloc(4, 1, TXRING_SZ, caps);
    if (!u->rxd || !u->txd || !u->rxbuf || !u->ring || !u->txring)
        goto fail;
    for (uint32_t i = 0; i < RXD_N; i++) {
        u->rxd[i].dw0.size = RXD_SZ;
        u->rxd[i].dw0.length = 0;
        u->rxd[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        u->rxd[i].buffer = u->rxbuf + i * RXD_SZ;
        u->rxd[i].next = &u->rxd[(i + 1u) % RXD_N];   /* circular: the DMA never runs out */
    }
    u->rx_next = 0;

    PERIPH_RCC_ATOMIC() {
        uhci_ll_enable_bus_clock(0, true);
        uhci_ll_reset_register(0);
    }
    uhci_dev_t *hw = UHCI_LL_GET_HW(0);
    uhci_ll_init(hw);   /* no packet header, no CRC, no SLIP framing */
    uhci_ll_attach_uart_port(hw, u->port);
    uhci_seper_chr_t sep = { 0 };
    uhci_ll_set_seper_chr(hw, &sep);
    uhci_swflow_ctrl_sub_chr_t flow = { 0 };
    uhci_ll_set_swflow_ctrl_sub_chr(hw, &flow);
    uhci_ll_rx_set_eof_mode(hw, UHCI_RX_IDLE_EOF);   /* line idle closes the current descriptor: low latency */

    gdma_channel_alloc_config_t ac = { .flags.isr_cache_safe = 1 };
    if (gdma_new_ahb_channel(&ac, &u->txch, &u->rxch) != ESP_OK)
        goto fail;
    gdma_connect(u->rxch, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));
    gdma_connect(u->txch, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));
    gdma_strategy_config_t st = { .owner_check = true, .auto_update_desc = true, .eof_till_data_popped = true };
    gdma_apply_strategy(u->rxch, &st);
    gdma_apply_strategy(u->txch, &st);
    size_t a_int = 0, a_ext = 0;
    gdma_get_alignment_constraints(u->txch, &a_int, &a_ext);
    if (a_int > 1)
        goto fail;   /* TX chunks start at any ring offset */
    gdma_rx_event_callbacks_t rcb = { .on_recv_eof = rx_isr, .on_recv_done = rx_isr, .on_descr_err = rx_err_isr };
    gdma_tx_event_callbacks_t tcb = { .on_trans_eof = tx_isr };
    if (gdma_register_rx_event_callbacks(u->rxch, &rcb, u) != ESP_OK ||
        gdma_register_tx_event_callbacks(u->txch, &tcb, u) != ESP_OK)
        goto fail;
    gdma_start(u->rxch, (intptr_t)&u->rxd[0]);
    return ESP_OK;
fail:
    dma_free(u);
    return ESP_FAIL;
}

/* ---- common --------------------------------------------------------------------------------------------------- */
esp_err_t esw_uart_init(esw_uart_t *u, uart_port_t port, uint32_t tx, uint32_t rx)
{
    memset(u, 0, sizeof(*u));
    u->port = port;
    u->tx = tx;
    u->rx = rx;
    portMUX_INITIALIZE(&u->lock);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, UART_PERIPH_SIGNAL(port, SOC_UART_PERIPH_SIGNAL_RX),
                                   false);
    return ESP_OK;
}

void esw_uart_stop(esw_uart_t *u)
{
    if (u->be == ESW_UART_BE_IRQ)
        (void)uart_driver_delete(u->port);   /* gates the UART clock: the caller unrouted TX before */
    else if (u->be == ESW_UART_BE_DMA)
        dma_free(u);
    u->be = ESW_UART_BE_OFF;
    u->evq = NULL;
}

esp_err_t esw_uart_start(esw_uart_t *u, uint32_t baud, uint32_t flags, esw_uart_be_t be, uint32_t *actual)
{
    uint32_t par = flags & ESW_UART_PARITY_MASK;
    const uint32_t known = ESW_UART_PARITY_MASK | ESW_UART_STOP2 | ESW_UART_RX_NOFILT | ESW_UART_TX_STRONG |
                           ESW_UART_NO_DMA;
    if (par == 3u || (flags & ~known) || baud < 300u || baud > 5000000u || be == ESW_UART_BE_OFF)
        return ESP_ERR_INVALID_ARG;
    esw_uart_stop(u);
    u->rh = u->rt = u->th = u->tt = u->tx_len = 0;
    u->tx_busy = 0;
    u->fifo_ovf = u->buf_full = u->frame_err = u->parity_err = u->brk = u->dma_err = 0;
    u->pend = 0;
    uart_config_t c = { .baud_rate = (int)baud, .data_bits = UART_DATA_8_BITS,
                        .parity = par == 1u ? UART_PARITY_EVEN : par == 2u ? UART_PARITY_ODD : UART_PARITY_DISABLE,
                        .stop_bits = (flags & ESW_UART_STOP2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    esp_err_t e = uart_param_config(u->port, &c);   /* also enables the UART clock */
    if (e != ESP_OK)
        return e;
    uart_dev_t *hw = UART_LL_GET_HW(u->port);
    /* RX glitch filter, in UART clock cycles (APB, 80 MHz): 1/8 bit, 4..255 (1 Mbaud: 10 cycles = 125 ns) */
    uint32_t filt = 80000000u / baud / 8u;
    filt = filt < 4u ? 4u : filt > 255u ? 255u : filt;
    hw->rx_filt.glitch_filt = filt;
    hw->rx_filt.glitch_filt_en = (flags & ESW_UART_RX_NOFILT) ? 0u : 1u;
    u->tx_strong = (flags & ESW_UART_TX_STRONG) ? 1u : 0u;
    gpio_ll_set_drive_capability(&GPIO, u->tx, u->tx_strong ? GPIO_DRIVE_CAP_2 : GPIO_DRIVE_CAP_0);
    if (be == ESW_UART_BE_IRQ) {
        e = uart_driver_install(u->port, (int)RING_SZ, (int)TXRING_SZ, 64, &u->evq,
                                ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3);
        if (e == ESP_OK)
            e = uart_set_rx_full_threshold(u->port, IRQ_RX_FULL);
        if (e == ESP_OK)
            e = uart_set_rx_timeout(u->port, IRQ_RX_TOUT);
        if (e != ESP_OK) {
            (void)uart_driver_delete(u->port);
            return e;
        }
        (void)uart_flush_input(u->port);
    } else {
        uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
        uart_ll_rxfifo_rst(hw);
        uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
        if (dma_start(u) != ESP_OK)
            return ESP_FAIL;
    }
    u->be = be;
    return uart_get_baudrate(u->port, actual);
}

size_t esw_uart_read(esw_uart_t *u, uint8_t *buf, size_t max)
{
    if (u->be == ESW_UART_BE_IRQ) {
        int n = uart_read_bytes(u->port, buf, (uint32_t)max, 0);
        return n > 0 ? (size_t)n : 0;
    }
    if (u->be != ESW_UART_BE_DMA)
        return 0;
    uint32_t avail = u->rh - u->rt, n = avail < max ? avail : (uint32_t)max;
    uint32_t off = u->rt & (RING_SZ - 1u), first = n < RING_SZ - off ? n : RING_SZ - off;
    memcpy(buf, u->ring + off, first);
    memcpy(buf + first, u->ring, n - first);
    u->rt += n;
    return n;
}

size_t esw_uart_write(esw_uart_t *u, const uint8_t *data, size_t n)
{
    if (u->be == ESW_UART_BE_IRQ) {
        size_t free_b = 0;
        (void)uart_get_tx_buffer_free_size(u->port, &free_b);
        n = n < free_b ? n : free_b;
        if (!n)
            return 0;
        int w = uart_write_bytes(u->port, data, n);
        return w > 0 ? (size_t)w : 0;
    }
    if (u->be != ESW_UART_BE_DMA)
        return 0;
    uint32_t free_b = TXRING_SZ - (u->th - u->tt);
    uint32_t m = n < free_b ? (uint32_t)n : free_b;
    uint32_t off = u->th & (TXRING_SZ - 1u), first = m < TXRING_SZ - off ? m : TXRING_SZ - off;
    memcpy(u->txring + off, data, first);
    memcpy(u->txring, data + first, m - first);
    portENTER_CRITICAL(&u->lock);
    u->th += m;
    tx_kick_locked(u);
    portEXIT_CRITICAL(&u->lock);
    return m;
}

int esw_uart_tx_idle(esw_uart_t *u)
{
    if (u->be == ESW_UART_BE_IRQ)
        return uart_wait_tx_done(u->port, 0) == ESP_OK;
    if (u->be == ESW_UART_BE_DMA)
        return u->th == u->tt && !u->tx_busy && uart_ll_is_tx_idle(UART_LL_GET_HW(u->port));
    return 1;
}

uint32_t esw_uart_poll(esw_uart_t *u)
{
    if (u->be == ESW_UART_BE_IRQ) {
        uart_event_t ev;
        while (xQueueReceive(u->evq, &ev, 0)) {
            switch (ev.type) {
            case UART_FIFO_OVF:    u->fifo_ovf++;   u->pend |= ESW_UART_RX_LOST;  break;
            case UART_BUFFER_FULL: u->buf_full++;   u->pend |= ESW_UART_RX_LOST;  break;
            case UART_FRAME_ERR:   u->frame_err++;  u->pend |= ESW_UART_RX_ERROR; break;
            case UART_PARITY_ERR:  u->parity_err++; u->pend |= ESW_UART_RX_ERROR; break;
            case UART_BREAK:       u->brk++;        u->pend |= ESW_UART_RX_BREAK; break;
            default: break;
            }
        }
    } else if (u->be == ESW_UART_BE_DMA) {
        uart_dev_t *hw = UART_LL_GET_HW(u->port);
        uint32_t raw = uart_ll_get_intraw_mask(hw) & LINE_ERRS;
        if (raw) {
            uart_ll_clr_intsts_mask(hw, raw);
            if (raw & UART_INTR_RXFIFO_OVF) {
                u->fifo_ovf++;
                u->pend |= ESW_UART_RX_LOST;
            }
            if (raw & UART_INTR_FRAM_ERR) {
                u->frame_err++;
                u->pend |= ESW_UART_RX_ERROR;
            }
            if (raw & UART_INTR_PARITY_ERR) {
                u->parity_err++;
                u->pend |= ESW_UART_RX_ERROR;
            }
            if (raw & UART_INTR_BRK_DET) {
                u->brk++;
                u->pend |= ESW_UART_RX_BREAK;
            }
        }
    }
    uint32_t f = u->pend;
    u->pend = 0;
    return f;
}

esp_err_t esw_uart_route(esw_uart_t *u)
{
    uint32_t sig_tx = UART_PERIPH_SIGNAL(u->port, SOC_UART_PERIPH_SIGNAL_TX);
    uint32_t sig_rx = UART_PERIPH_SIGNAL(u->port, SOC_UART_PERIPH_SIGNAL_RX);
    /* TX: the pad already drives the idle level (GPIO high from the board's prepare step, or the UART itself).
     * Switch the matrix source first, then make sure the output stays enabled from GPIO_ENABLE. */
    gpio_ll_func_sel(&GPIO, (uint8_t)u->tx, PIN_FUNC_GPIO);
    gpio_ll_set_output_signal_matrix_source(&GPIO, u->tx, sig_tx, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)u->tx, false, false);
    gpio_ll_set_drive_capability(&GPIO, u->tx, u->tx_strong ? GPIO_DRIVE_CAP_2 : GPIO_DRIVE_CAP_0);
    gpio_ll_output_enable(&GPIO, u->tx);
    /* RX: input buffer on, then connect it to the UART (the board pull-up keeps it idle high) */
    gpio_ll_func_sel(&GPIO, (uint8_t)u->rx, PIN_FUNC_GPIO);
    gpio_ll_input_enable(&GPIO, u->rx);
    gpio_ll_set_input_signal_matrix_source(&GPIO, sig_rx, u->rx, false);
    if (GPIO.func_out_sel_cfg[u->tx].func_sel != sig_tx || GPIO.func_in_sel_cfg[sig_rx].func_sel != u->rx ||
        !GPIO.func_in_sel_cfg[sig_rx].sig_in_sel)
        return ESP_FAIL;
    return ESP_OK;
}

void esw_uart_unroute(esw_uart_t *u)
{
    gpio_ll_set_level(&GPIO, u->tx, 1);   /* the plain GPIO output takes over at the idle level: no edge */
    gpio_ll_set_output_signal_matrix_source(&GPIO, u->tx, SIG_GPIO_OUT_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   UART_PERIPH_SIGNAL(u->port, SOC_UART_PERIPH_SIGNAL_RX), false);
}
