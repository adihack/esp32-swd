/* SPDX-License-Identifier: MIT */
/* nRF52840 UART test firmware for esp-swd hardware tests. Bare metal, no SDK, polling, no interrupts except SysTick.
 *
 * UART0 (legacy, non-DMA): TX on P1.02 (DK Arduino header D1), RX on P1.01 (D0, internal pull-up), 8N1, 115200
 * after reset, HFXO clock for accurate baud rates. Actual nRF rates are 16 MHz / k: 115200 -> 115108,
 * 460800 -> 457143, 921600 -> 941176 (2 % off: avoid), 1000000 exact.
 *
 * Boot banner:      "ESWTEST nrf52840 uart 1 resetreas=0x<hex>"
 * Line commands (end with \r or \n; one reply line each, "err" if unknown):
 *   ping            -> "pong"
 *   baud <n>        -> "ok baud <n>", then the UART switches to n (1200..1000000)
 *   echo <n>        -> "ok echo <n>", echoes the next n raw bytes, then "done" ("timeout <got>" after 1 s silence,
 *                      "overflow <pos>" if the echo fell 16 KB behind: this UART is single-buffered, so its TX runs
 *                      a few percent below line rate, about 7 % at 1 Mbaud)
 *   send <n> <seed> -> "ok send <n>", n bytes of a xorshift32 stream (each step 4 bytes, little endian), "done"
 *   sink <n>        -> "ok sink <n>", reads n raw bytes, then "crc <8 hex>" (zlib.crc32), or "timeout <got>"
 *   tick <ms>       -> "ok tick", then "tick <k> <uptime_ms>" every <ms> ms until the next received byte
 *   duplex <n> <seed_tx> <seed_rx>
 *                   -> "ok duplex <n>", then sends n stream bytes (seed_tx) while it receives n bytes and checks
 *                      them against the stream of seed_rx; then "dup got=<n> err=<n> [<pos>:<xor hex> ...]"
 *                      (first 8 bad bytes), or with "timeout" when the host stopped sending for 1 s
 *   stat            -> "stat rx=<n> ovr=<n> par=<n> frm=<n> brk=<n>"   (UART receive errors since reset)
 * LED1 blinks at 1 Hz while the firmware runs.
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define APPROTECT_DISABLE  REG(0x40000558u)
#define CLOCK_HFCLKSTART   REG(0x40000000u)
#define CLOCK_HFCLKSTARTED REG(0x40000100u)
#define POWER_RESETREAS    REG(0x40000400u)

#define UART0       0x40002000u
#define U_STARTRX   REG(UART0 + 0x000u)
#define U_STARTTX   REG(UART0 + 0x008u)
#define U_RXDRDY    REG(UART0 + 0x108u)
#define U_TXDRDY    REG(UART0 + 0x11Cu)
#define U_ERROR     REG(UART0 + 0x124u)
#define U_ERRORSRC  REG(UART0 + 0x480u)
#define U_ENABLE    REG(UART0 + 0x500u)
#define U_PSEL_RTS  REG(UART0 + 0x508u)
#define U_PSEL_TXD  REG(UART0 + 0x50Cu)
#define U_PSEL_CTS  REG(UART0 + 0x510u)
#define U_PSEL_RXD  REG(UART0 + 0x514u)
#define U_RXD       REG(UART0 + 0x518u)
#define U_TXD       REG(UART0 + 0x51Cu)
#define U_BAUDRATE  REG(UART0 + 0x524u)
#define U_CONFIG    REG(UART0 + 0x56Cu)

#define P0 0x50000000u
#define P1 0x50000300u
#define GPIO_OUTSET(p)     REG((p) + 0x508u)
#define GPIO_OUTCLR(p)     REG((p) + 0x50Cu)
#define GPIO_DIRSET(p)     REG((p) + 0x518u)
#define GPIO_PIN_CNF(p, n) REG((p) + 0x700u + 4u * (n))

#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)

#define PIN_TX 2u    /* P1.02 */
#define PIN_RX 1u    /* P1.01 */
#define LED1 13u     /* P0.13, active low */

static volatile uint32_t g_ms;
static uint32_t g_rx, g_ovr, g_par, g_frm, g_brk;
static uint32_t g_tick_ms, g_tick_next, g_tick_n;

void SysTick_Handler(void)
{
    g_ms++;
}

/* ---- UART ---------------------------------------------------------------------------------------------- */
static void uart_setup(uint32_t baud)
{
    uint64_t r = ((uint64_t)baud << 32) / 16000000u;   /* BAUDRATE = baud * 2^32 / 16 MHz, 4 KiB steps */
    U_ENABLE = 0;
    U_BAUDRATE = (uint32_t)((r + 0x800u) & 0xFFFFF000u);
    U_ENABLE = 4;
    U_STARTRX = 1;
    U_STARTTX = 1;
}

static void poll_err(void)
{
    if (U_ERROR) {
        U_ERROR = 0;
        uint32_t s = U_ERRORSRC;
        U_ERRORSRC = s;   /* write 1 to clear */
        g_ovr += s & 1u;
        g_par += (s >> 1) & 1u;
        g_frm += (s >> 2) & 1u;
        g_brk += (s >> 3) & 1u;
    }
}

static int rx_byte(void)
{
    poll_err();
    if (!U_RXDRDY)
        return -1;
    U_RXDRDY = 0;   /* clear first: reading RXD pops the FIFO and may raise the next event */
    g_rx++;
    return (int)(U_RXD & 0xFFu);
}

static void tx_byte(uint8_t b)
{
    U_TXD = b;
    while (!U_TXDRDY) {
    }
    U_TXDRDY = 0;
}

static void puts_(const char *s)
{
    while (*s)
        tx_byte((uint8_t)*s++);
}

static void putu(uint32_t v)
{
    char b[11];
    int i = 10;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    puts_(b + i);
}

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        tx_byte((uint8_t)"0123456789abcdef"[(v >> i) & 15u]);
}

static void wait_ms(uint32_t ms)
{
    uint32_t t0 = g_ms;
    while (g_ms - t0 < ms) {
    }
}

/* ---- test modes ----------------------------------------------------------------------------------------- */
static uint8_t g_ring[16384];

/* Echo n bytes. The single-buffered TX is slower than RX at high rates: the ring holds the backlog. Returns the
 * bytes received, or (n + 1 + position) when the ring overflowed. */
static uint32_t echo(uint32_t n)
{
    uint32_t head = 0, tail = 0, got = 0, busy = 0, last = g_ms;
    while (tail < n) {
        if (got < n) {
            int c = rx_byte();
            if (c >= 0) {
                if (head - tail >= sizeof(g_ring))
                    return n + 1u + got;
                g_ring[head++ & (sizeof(g_ring) - 1u)] = (uint8_t)c;
                got++;
                last = g_ms;
            } else if (head == tail && g_ms - last > 1000u) {
                break;
            }
        }
        if (busy && U_TXDRDY) {
            U_TXDRDY = 0;
            busy = 0;
        }
        if (!busy && tail != head) {
            U_TXD = g_ring[tail++ & (sizeof(g_ring) - 1u)];
            busy = 1;
        }
    }
    if (busy) {
        while (!U_TXDRDY) {
        }
        U_TXDRDY = 0;
    }
    return got;
}

static void send(uint32_t n, uint32_t x)
{
    if (!x)
        x = 1;
    while (n) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        for (uint32_t k = 0; k < 4u && n; k++, n--)
            tx_byte((uint8_t)(x >> (8u * k)));
    }
}

static uint32_t sink(uint32_t n, uint32_t *crc_out)
{
    uint32_t crc = 0xFFFFFFFFu, got = 0, last = g_ms;
    while (got < n) {
        int c = rx_byte();
        if (c < 0) {
            if (g_ms - last > 1000u)
                break;
            continue;
        }
        last = g_ms;
        got++;
        crc ^= (uint32_t)c;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    *crc_out = ~crc;
    return got;
}

/* Full duplex: both directions at line rate with independent data, so a corrupted byte tells which way it went. */
static uint32_t xs_next(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x;
}

static void duplex(uint32_t n, uint32_t xt, uint32_t xr)
{
    uint32_t got = 0, sent = 0, busy = 0, err = 0, last = g_ms, wt = 0, kt = 4, wr = 0, kr = 4, timeout = 0;
    uint32_t bad_pos[8], bad_xor[8];
    if (!xt)
        xt = 1;
    if (!xr)
        xr = 1;
    while (got < n || sent < n || busy) {
        int c = got < n ? rx_byte() : -1;
        if (c >= 0) {
            if (kr == 4) {
                wr = xs_next(&xr);
                kr = 0;
            }
            uint32_t want = (wr >> (8u * kr++)) & 0xFFu;
            if ((uint32_t)c != want) {
                if (err < 8u) {
                    bad_pos[err] = got;
                    bad_xor[err] = (uint32_t)c ^ want;
                }
                err++;
            }
            got++;
            last = g_ms;
        } else if (got < n && sent == n && !busy && g_ms - last > 1000u) {
            timeout = 1;
            break;
        }
        if (busy && U_TXDRDY) {
            U_TXDRDY = 0;
            busy = 0;
        }
        if (!busy && sent < n) {
            if (kt == 4) {
                wt = xs_next(&xt);
                kt = 0;
            }
            U_TXD = (wt >> (8u * kt++)) & 0xFFu;
            busy = 1;
            sent++;
        }
    }
    puts_("dup got=");
    putu(got);
    puts_(" err=");
    putu(err);
    for (uint32_t i = 0; i < err && i < 8u; i++) {
        puts_(" ");
        putu(bad_pos[i]);
        puts_(":");
        puthex(bad_xor[i]);
    }
    if (timeout)
        puts_(" timeout");
    puts_("\r\n");
}

/* ---- commands ------------------------------------------------------------------------------------------- */
static const char *word(const char *s, const char *w)
{
    while (*w)
        if (*s++ != *w++)
            return 0;
    return (*s == ' ' || *s == 0) ? s : 0;
}

static uint32_t num(const char **s)
{
    uint32_t v = 0;
    while (**s == ' ')
        (*s)++;
    while (**s >= '0' && **s <= '9')
        v = v * 10u + (uint32_t)(*(*s)++ - '0');
    return v;
}

static void command(const char *s)
{
    const char *p;
    if ((p = word(s, "ping"))) {
        puts_("pong\r\n");
    } else if ((p = word(s, "baud"))) {
        uint32_t b = num(&p);
        if (b < 1200u || b > 1000000u) {
            puts_("err baud\r\n");
            return;
        }
        puts_("ok baud ");
        putu(b);
        puts_("\r\n");
        wait_ms(2);   /* last stop bit out */
        uart_setup(b);
    } else if ((p = word(s, "echo"))) {
        uint32_t n = num(&p);
        puts_("ok echo ");
        putu(n);
        puts_("\r\n");
        uint32_t got = echo(n);
        if (got == n) {
            puts_("done\r\n");
        } else if (got > n) {
            puts_("overflow ");
            putu(got - n - 1u);
            puts_("\r\n");
        } else {
            puts_("timeout ");
            putu(got);
            puts_("\r\n");
        }
    } else if ((p = word(s, "send"))) {
        uint32_t n = num(&p), seed = num(&p);
        puts_("ok send ");
        putu(n);
        puts_("\r\n");
        send(n, seed);
        puts_("done\r\n");
    } else if ((p = word(s, "sink"))) {
        uint32_t n = num(&p), crc = 0;
        puts_("ok sink ");
        putu(n);
        puts_("\r\n");
        uint32_t got = sink(n, &crc);
        if (got == n) {
            puts_("crc ");
            puthex(crc);
        } else {
            puts_("timeout ");
            putu(got);
        }
        puts_("\r\n");
    } else if ((p = word(s, "duplex"))) {
        uint32_t n = num(&p), st = num(&p), sr = num(&p);
        puts_("ok duplex ");
        putu(n);
        puts_("\r\n");
        duplex(n, st, sr);
    } else if ((p = word(s, "tick"))) {
        g_tick_ms = num(&p);
        g_tick_next = g_ms;
        puts_("ok tick\r\n");
    } else if ((p = word(s, "stat"))) {
        puts_("stat rx=");
        putu(g_rx);
        puts_(" ovr=");
        putu(g_ovr);
        puts_(" par=");
        putu(g_par);
        puts_(" frm=");
        putu(g_frm);
        puts_(" brk=");
        putu(g_brk);
        puts_("\r\n");
    } else {
        puts_("err\r\n");
    }
}

/* ---- startup -------------------------------------------------------------------------------------------- */
extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

int main(void)
{
    CLOCK_HFCLKSTARTED = 0;
    CLOCK_HFCLKSTART = 1;   /* HFXO: the baud rate follows the crystal, not the RC oscillator */
    while (!CLOCK_HFCLKSTARTED) {
    }
    uint32_t rr = POWER_RESETREAS;
    POWER_RESETREAS = rr;   /* write 1 to clear: the next banner shows only the next reset's cause */

    GPIO_OUTSET(P0) = 1u << LED1;
    GPIO_DIRSET(P0) = 1u << LED1;
    GPIO_OUTSET(P1) = 1u << PIN_TX;   /* idle high while the UART is disabled (baud changes) */
    GPIO_DIRSET(P1) = 1u << PIN_TX;
    GPIO_PIN_CNF(P1, PIN_RX) = 3u << 2;   /* input, buffer connected, pull-up: an isolated line reads idle */
    U_PSEL_TXD = (1u << 5) | PIN_TX;
    U_PSEL_RXD = (1u << 5) | PIN_RX;
    U_PSEL_RTS = 0xFFFFFFFFu;
    U_PSEL_CTS = 0xFFFFFFFFu;
    U_CONFIG = 0;   /* 8N1, no flow control */
    uart_setup(115200u);

    SYST_RVR = 64000u - 1u;   /* 1 ms at 64 MHz */
    SYST_CVR = 0;
    SYST_CSR = 7u;

    puts_("\r\nESWTEST nrf52840 uart 1 resetreas=0x");
    puthex(rr);
    puts_("\r\n");

    char line[64];
    uint32_t len = 0, led_next = 0, led = 0;
    for (;;) {
        uint32_t now = g_ms;
        if ((int32_t)(now - led_next) >= 0) {
            led_next = now + 500u;
            led ^= 1u;
            if (led)
                GPIO_OUTCLR(P0) = 1u << LED1;
            else
                GPIO_OUTSET(P0) = 1u << LED1;
        }
        if (g_tick_ms && (int32_t)(now - g_tick_next) >= 0) {
            g_tick_next += g_tick_ms;
            puts_("tick ");
            putu(g_tick_n++);
            puts_(" ");
            putu(now);
            puts_("\r\n");
        }
        int c = rx_byte();
        if (c < 0)
            continue;
        g_tick_ms = 0;   /* any input stops the ticks */
        if (c == '\r' || c == '\n') {
            if (len) {
                line[len] = 0;
                command(line);
                len = 0;
            }
        } else if (len < sizeof(line) - 1u) {
            line[len++] = (char)c;
        }
    }
}

void Reset_Handler(void)
{
    APPROTECT_DISABLE = 0x5Au;   /* first of all: nRF52840 build code F re-locks the debug port at every reset unless
                                    the firmware opens it (UICR.APPROTECT = HwDisabled is set by esp-swd unlock) */
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;)
        *dst++ = 0;
    main();
    for (;;) {
    }
}

static void Default_Handler(void)
{
    for (;;) {
    }
}

__attribute__((section(".vectors"), used)) static void (*const g_vectors[16])(void) = {
    [0] = (void (*)(void))(uintptr_t)&_estack,
    [1] = Reset_Handler,
    [2 ... 14] = Default_Handler,
    [15] = SysTick_Handler,
};
