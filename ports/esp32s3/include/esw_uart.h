/* SPDX-License-Identifier: MIT */
/* esp-swd ESP32-S3 port: DUT UARTs with two receive back-ends behind one interface.
 *
 *   DMA:  UART -> UHCI -> GDMA into a circular chain of 32 x 256 B descriptors that never stops (a line-idle EOF
 *         just closes the current descriptor early). The GDMA interrupt moves finished descriptors into a 32 KB
 *         software ring. The hardware alone buffers 8 KB (80 ms at 1 Mbaud) before any CPU must react. TX also by
 *         DMA from an 8 KB ring. The ESP32-S3 has one UHCI, so one channel at a time.
 *   IRQ:  the ESP-IDF UART driver, hardened: ISR in IRAM (runs while the flash cache is off), interrupt level 3,
 *         RX-FIFO-full threshold 32 of 128 bytes (96 bytes = 0.96 ms of margin at 1 Mbaud).
 * Both: 32 KB RX ring, 8 KB TX ring. Start, stop and the GDMA/UART interrupts belong to one core (the caller of
 * esw_uart_start); esw_uart_write may be called from any core.
 *
 * Pads: the board's isolation layer owns them. Every time a UART switch group closes, its prepare step puts TX on
 * a plain GPIO driven high (idle) and RX on a plain input. esw_uart_route() then hands both pads to the UART
 * through the GPIO matrix: only the matrix source changes, the TX output stays enabled and both sources idle high,
 * so the DUT never sees an edge (uart_set_pin() would first release TX and float the line). esw_uart_unroute()
 * does the reverse; call it before a stop, which gates the UART clock.
 *
 * Signal integrity, measured on r5 with jumper wires to an nRF52840 DK: at full drive strength our TX edges
 * coupled into our RX and the ESP32-S3 receiver sampled the glitches as bits (DK -> tool only). Defaults: TX at
 * the weakest drive strength and the RX glitch filter at 1/8 bit; either alone fixed it.
 */
#ifndef ESW_UART_H
#define ESW_UART_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hal/dma_types.h"
#include "esp_private/gdma.h"

typedef enum { ESW_UART_BE_OFF = 0, ESW_UART_BE_IRQ = 1, ESW_UART_BE_DMA = 2 } esw_uart_be_t;

typedef struct {
    uart_port_t port;
    uint32_t tx, rx;            /* GPIO numbers */
    esw_uart_be_t be;
    uint8_t tx_strong;          /* TX pad drive strength: 0 weakest (default), 1 full */
    portMUX_TYPE lock;          /* DMA TX state, shared by writers (any core) and the GDMA ISR */
    /* IRQ back-end */
    QueueHandle_t evq;
    /* DMA back-end */
    gdma_channel_handle_t rxch, txch;
    dma_descriptor_t *rxd, *txd;
    uint8_t *rxbuf, *ring, *txring;
    uint32_t rx_next;                       /* next RX descriptor to drain (ISR) */
    volatile uint32_t rh, rt;               /* RX ring: head (ISR), tail (reader) */
    volatile uint32_t th, tt, tx_len;       /* TX ring: head (writer), tail (done), in-flight length */
    volatile uint8_t tx_busy;
    /* counters: line errors are counted per poll in which the hardware flag was seen (DMA back-end) */
    volatile uint32_t fifo_ovf, buf_full, frame_err, parity_err, brk, dma_err;
    volatile uint8_t pend;      /* ESW_UART_RX_* since the last esw_uart_poll() */
} esw_uart_t;

/* Once at boot: remembers port and pads, ties the UART's RX input to a constant high (idle). */
esp_err_t esw_uart_init(esw_uart_t *u, uart_port_t port, uint32_t tx, uint32_t rx);

/* (Re)start with a back-end: stops the current one first. 8 data bits; flags: ESW_UART_PARITY_MASK,
 * ESW_UART_STOP2, ESW_UART_RX_NOFILT, ESW_UART_TX_STRONG; baud 300..5000000. Discards received data and resets
 * the counters. *actual: the baud rate the divider produces. Unroute first if the pads may be routed. */
esp_err_t esw_uart_start(esw_uart_t *u, uint32_t baud, uint32_t flags, esw_uart_be_t be, uint32_t *actual);
void esw_uart_stop(esw_uart_t *u);

size_t esw_uart_read(esw_uart_t *u, uint8_t *buf, size_t max);
size_t esw_uart_write(esw_uart_t *u, const uint8_t *data, size_t n);   /* accepted bytes; never blocks */
int esw_uart_tx_idle(esw_uart_t *u);                                   /* everything written is on the wire */
/* Collect error events (DMA back-end: from the UART's raw interrupt flags). Returns the ESW_UART_RX_* flags
 * gathered since the previous call. */
uint32_t esw_uart_poll(esw_uart_t *u);

esp_err_t esw_uart_route(esw_uart_t *u);
void esw_uart_unroute(esw_uart_t *u);   /* TX back to a plain GPIO driven high, RX input to constant high */

#endif
