/*
 * Interactive pin-debug console — wiring verification and ISP-routing
 * discovery WITHOUT a multimeter fighting oxidized joints.
 *
 * Entry: send the ASCII token "@PINDBG!" on the transport (any terminal at
 * 115200, e.g. picocom).  Matched between STK500v2 frames only, so the
 * token appearing inside programming data cannot trigger it.
 * All GPIO drop to quasi-bidir with latches in the
 * SAFE state (VCC off, RESET node at GND, everything else weak-high pullup),
 * so both probing directions work:
 *   - drive a pin, meter the header/ZIF side;
 *   - ground a header pin, hit 'p' and see which port bit fell.
 *
 * Commands (single ASCII chars, echoed; \r\n friendly):
 *   0..7  select DATA0..7          a XA0    b XA1    c BS1    d BS2
 *   e OE    f WR    g PAGEL  h XTAL1(CLK)   r RESET_DRV  v VCC_EN  l LED
 *   x P3.3  y P3.4  z P3.7   w P1.7
 *   then '+' = latch high, '-' = latch low (quasi: strong low, weak high)
 *   p  print all port input values
 *   ?  this pin map
 *   B  jump to ROM USB bootloader
 *   q  quit back to STK500v2 (restores normal GPIO init + safe idle)
 *
 * Analog (see adc.h for the channel map):
 *   M  own-VCC in mV (bandgap) + sag-guard state
 *   A  ADC-probe the selected pin: float it, read node mV, restore quasi
 *      ('r' reads the RESET_DRV base network: ~2.5 V = NPN net healthy)
 *   U  probe the UART2-header P1.0 pad (ch0) — jumper it to the socket
 *      VCC rail for real rail/clamp-Vf measurements on USB transport
 *   W  capture 256 x ~1ms of the selected pin's channel (VCC if none),
 *      print mV — power-on wobble / sag transients
 *   G  toggle the VCC sag guard, clear a latched fault
 *
 * DANGER notes (expert tool, no interlocks): 'r-' turns the reset NPN off
 * -> 12 V ON THE RESET NODE; 'v+' powers the socket.  'v+'/'v-' bypass
 * hal_vcc(), so the sag guard is NOT armed for manual socket power.
 */

#include "board.h"
#include "hal.h"
#include "uart2.h"
#include "stk500v2.h"
#include "adc.h"
#include "pindbg.h"

static const char entry_token[] = "@PINDBG!";
static uint8_t tok_ix;
static __bit active;
static uint8_t sel;            /* selected pin command char, 0 = none */

static void
puts2(const char *s)
{
    while (*s)
        hal_tx_byte((uint8_t)*s++);
    hal_tx_flush();
}

static void
put_hex(uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    hal_tx_byte((uint8_t)hx[v >> 4]);
    hal_tx_byte((uint8_t)hx[v & 0x0F]);
}

static void
put_u16(uint16_t v)
{
    static __xdata char b[5];
    uint8_t i = 0;

    do {
        b[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        hal_tx_byte((uint8_t)b[--i]);
}

/* Map a command char to port number + bit mask; 0xFF port = no such pin. */
static uint8_t
pin_lookup(uint8_t c, uint8_t *mask)
{
    uint8_t port = 0xFF;

    if (c >= '0' && c <= '7') {                 /* DATA on P2 */
        *mask = (uint8_t)(1u << (c - '0'));
        return 2;
    }
    switch (c) {
    case 'a': port = 3; *mask = 0x20; break;    /* XA0   P3.5 */
    case 'b': port = 0; *mask = 0x04; break;    /* XA1   P0.2 */
    case 'c': port = 3; *mask = 0x40; break;    /* BS1   P3.6 */
    case 'd': port = 1; *mask = 0x40; break;    /* BS2 dummy = red LED P1.6 */
    case 'e': port = 1; *mask = 0x80; break;    /* OE    P1.7 (PROVEN) */
    case 'f': port = 1; *mask = 0x08; break;    /* WR    P1.3 (PROVEN) */
    case 'g': port = 0; *mask = 0x08; break;    /* PAGEL P0.3 */
    case 'h': port = 0; *mask = 0x01; break;    /* XTAL1 P0.0 */
    case 'r': port = 5; *mask = 0x10; break;    /* RESET_DRV P5.4 */
    case 'v': port = 1; *mask = 0x10; break;    /* VCC_EN P1.4 (LOW = on!) */
    case 'l': port = 1; *mask = 0x20; break;    /* P1.5 = ISP-header RST
                                                   (100R; NOT on the ZIF) */
    case 'x': port = 3; *mask = 0x08; break;    /* P3.3 (unknown) */
    case 'y': port = 3; *mask = 0x10; break;    /* P3.4 (unknown) */
    case 'z': port = 3; *mask = 0x80; break;    /* P3.7 (test point) */
    case 'w': port = 1; *mask = 0x80; break;    /* P1.7 (unknown) */
    default: break;
    }
    return port;
}

static void
pin_write(uint8_t port, uint8_t mask, uint8_t level)
{
    switch (port) {
    case 0: if (level) P0 |= mask; else P0 &= (uint8_t)~mask; break;
    case 1: if (level) P1 |= mask; else P1 &= (uint8_t)~mask; break;
    case 2: if (level) P2 |= mask; else P2 &= (uint8_t)~mask; break;
    case 3: if (level) P3 |= mask; else P3 &= (uint8_t)~mask; break;
    case 5: if (level) P5 |= mask; else P5 &= (uint8_t)~mask; break;
    default: break;
    }
}

/* ---- analog probes ------------------------------------------------------ */

/* ADC channel for a (port, single-bit mask) pin; 0xFF = no channel.
 * P1.n = ADCn, P0.n = ADC8+n, P5.4 = ADC2 (RESET_DRV base!); P2/P3 none. */
static uint8_t
pin_adc_ch(uint8_t port, uint8_t mask)
{
    uint8_t b = 0;

    while (!(mask & 1)) {
        mask >>= 1;
        ++b;
    }
    if (port == 1)
        return b;
    if (port == 0)
        return (uint8_t)(8 + b);
    if (port == 5 && b == 4)
        return ADC_CH_RSTDRV;
    return 0xFF;
}

/* Float the pin (true hi-Z) so its external network sets the level. */
static void
pin_float(uint8_t port, uint8_t mask)
{
    switch (port) {
    case 0: P0M1 |= mask; P0M0 &= (uint8_t)~mask; break;
    case 1: P1M1 |= mask; P1M0 &= (uint8_t)~mask; break;
    case 5: P5M1 |= mask; P5M0 &= (uint8_t)~mask; break;
    default: break;
    }
}

/* Back to the PINDBG quasi idle (TxD2/VCC_EN stay strong, latch unchanged). */
static void
pin_unfloat(uint8_t port, uint8_t mask)
{
    switch (port) {
    case 0: P0M1 &= (uint8_t)~mask; break;
    case 1: P1M1 &= (uint8_t)~mask; P1M0 |= (uint8_t)(mask & 0x16); break;
    case 5: P5M1 &= (uint8_t)~mask; break;
    default: break;
    }
}

static void
adc_probe(uint8_t port, uint8_t mask)
{
    uint8_t ch = (port == 0xFF) ? 0xFF : pin_adc_ch(port, mask);
    uint16_t mv;

    if (ch == 0xFF) {
        puts2(" no adc on pin\r\n");
        return;
    }
    pin_float(port, mask);
    hal_delay_ms(2);            /* node settles through its own network */
    mv = adc_ch_mv(ch);
    pin_unfloat(port, mask);
    puts2(" ");
    put_u16(mv);
    puts2("mV\r\n");
}

/* 256 samples, ~1 ms pitch: selected pin's channel (floated for the whole
 * window) or our own VCC via the bandgap when no ADC pin is selected. */
static __xdata uint16_t wave[256];

static void
wave_capture(void)
{
    static __xdata uint16_t vcc, i, mv;
    uint8_t ch = ADC_CH_BGV, port = 0xFF, mask = 0;

    if (sel) {
        port = pin_lookup(sel, &mask);
        if (port != 0xFF) {
            ch = pin_adc_ch(port, mask);
            if (ch == 0xFF) {
                ch = ADC_CH_BGV;
                port = 0xFF;
            }
        }
    }
    vcc = adc_vcc_mv();
    if (port != 0xFF)
        pin_float(port, mask);
    for (i = 0; i < 256; ++i) {
        wave[i] = adc_read(ch);
        hal_delay_ms(1);
    }
    if (port != 0xFF)
        pin_unfloat(port, mask);

    puts2("ch");
    put_u16(ch);
    puts2(" 256x~1ms mV:\r\n");
    for (i = 0; i < 256; ++i) {
        if (ch == ADC_CH_BGV)
            mv = wave[i] ? (uint16_t)(4874240UL / wave[i]) : 0;
        else
            mv = (uint16_t)(((uint32_t)wave[i] * vcc) >> 12);
        put_u16(mv);
        if ((i & 15) == 15)
            puts2("\r\n");
        else
            hal_tx_byte(' ');
    }
    puts2("CAPTURE DONE\r\n");
}

/* Deterministic TX test pattern: same shape as the W dump (put_u16 +
 * space, \r\n flush every 16) but with known values — an exact host-side
 * diff isolates transport corruption from measurement. */
static void
tx_pattern(void)
{
    uint16_t i;

    puts2("txpat 0..255:\r\n");
    for (i = 0; i < 256; ++i) {
        put_u16((uint16_t)(2000 + i));
        if ((i & 15) == 15)
            puts2("\r\n");
        else
            hal_tx_byte(' ');
    }
    puts2("PATTERN DONE\r\n");
}

static void
print_ports(void)
{
    puts2("P0="); put_hex(P0);
    puts2(" P1="); put_hex(P1);
    puts2(" P2="); put_hex(P2);
    puts2(" P3="); put_hex(P3);
    puts2(" P5="); put_hex(P5);
    puts2("\r\n");
}

static void
enter(void)
{
    /* Everything quasi-bidir (weak-high input / strong-low drive)... */
    P0M1 = 0; P0M0 = 0;
    P1M1 = 0; P1M0 = 0x16;      /* TxD2 + VCC_EN strong (bit2 pad-less/inert) */
    P2M1 = 0; P2M0 = 0;
    P3M1 = 0x03; P3M0 = 0;      /* USB D-/D+ stay hi-Z (PHY owns them) */
    P5M1 = 0; P5M0 = 0;
    /* ...latches: pullups everywhere; VCC_EN (P1.4) HIGH = socket off,
     * RESET_DRV high = RESET node at GND. */
    P0 = 0xFF;
    P1 = 0xFF;
    P2 = 0xFF;
    P3 = 0xFF;
    P5 |= 0x10;
    active = 1;
    sel = 0;
    puts2("\r\nPINDBG (?=map p=ports +/-=set B=bootldr q=quit)\r\n");
}

static void
leave(void)
{
    active = 0;
    /* Same safe-idle order as boot: latches first, then modes, then the
     * released idle (no push-pull drive into the unpowered socket). */
    P1 |= 0x10;                     /* VCC_EN high = socket off */
    P5 |= 0x10;                     /* RESET at GND */
    P2 = 0x00;
    P0 &= (uint8_t)~0x01;           /* XTAL1 latch low */
    BOARD_GPIO_INIT();
    BOARD_CTRL_RELEASE();
    BOARD_DATA_IN();
    puts2("BYE\r\n");
}

static void
help(void)
{
    puts2("0-7 DATA  a XA0 b XA1 c BS1 d BS2 e OE f WR g PAGEL h XTAL1\r\n"
          "r RSTDRV(- = 12V!) v VCCEN(- = on) l ISPRST x P3.3 y P3.4 z P3.7 w P1.7\r\n"
          "S ISP pin scan (chip in socket; finds true SCK/MISO)\r\n"
          "C clamp-decay probe (charge, hi-Z, per-pin fall time ~0.1ms ticks)\r\n"
          "V VCC_EN hunt (find which pin actually powers the socket)\r\n"
          "R read selftest USI-SPI stream (SCK=XA1, MISO=XA0; chip running)\r\n"
          "M VCC mV+guard  A adc-probe sel pin  U probe hdr P1.0\r\n"
          "W 256x1ms capture (sel pin or VCC)  G sag-guard toggle\r\n");
}

/*
 * ISP-entry pin scan v2: full MOSI x SCK matrix — the socketed AVR names
 * its own wires without trusting ANY traced control-pin assignment.
 *
 * For every ordered pair (MOSI, SCK) of candidate pins (P0.0-.3,
 * P1.3/.5/.6/.7, P3.3-.7 = every drivable header suspect): power-cycle
 * the socket (RESET stays held at GND) and clock the Programming Enable
 * sequence AC 53 00 00.  A synced AVR echoes 0x53 on its MISO (PB1 ->
 * XA0 header) while the 3rd byte shifts; sampling ALL ports in that
 * window means one hit names MOSI, SCK and MISO together and proves
 * VCC + GND + RESET-at-GND end to end.  ~156 combos, ~30 s.
 */
static const uint8_t scan_port[14] =
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3 };
static const uint8_t scan_bit[14]  =
    { 0, 1, 2, 3, 2, 3, 5, 6, 7, 3, 4, 5, 6, 7 };

static void
put_pin(uint8_t port, uint8_t bit)
{
    hal_tx_byte('P');
    hal_tx_byte((uint8_t)('0' + port));
    hal_tx_byte('.');
    hal_tx_byte((uint8_t)('0' + bit));
}

static void
isp_scan(void)
{
    static const uint8_t seq[4] = { 0xAC, 0x53, 0x00, 0x00 };
    static __xdata uint8_t smp[4][8];    /* all ports at each byte-2 clock */
    static __xdata uint8_t mport, mmask, sport, smask, mask, hits;
    static __xdata uint8_t mi, si, b, port, bit;
    uint8_t i;                           /* bit-bang loop stays fast */

    hits = 0;
    for (mi = 0; mi < 14; ++mi) {
        mport = scan_port[mi];
        mmask = (uint8_t)(1u << scan_bit[mi]);
        puts2("MOSI="); put_pin(mport, scan_bit[mi]); puts2(":");

        for (si = 0; si < 14; ++si) {
            if (si == mi)
                continue;
            sport = scan_port[si];
            smask = (uint8_t)(1u << scan_bit[si]);

            /* socket off (P1.4 high); idle latches high, MOSI + SCK low
             * before power; then REAL rail via the P1.4 active-low switch */
            P1 |= 0x10;                         /* VCC off */
            P0 |= 0x0F; P3 |= 0xF8; P1 |= 0xEC; P2 = 0xFF;
            pin_write(mport, mmask, 0);
            pin_write(sport, smask, 0);         /* SCK low BEFORE power */
            hal_delay_ms(60);                   /* rail drain */
            P1 &= (uint8_t)~0x10;               /* VCC ON */
            hal_delay_ms(25);

            for (i = 0; i < 32; ++i) {
                pin_write(mport, mmask,
                          (uint8_t)((seq[i >> 3] >> (7 - (i & 7))) & 1));
                hal_delay_us(10);
                pin_write(sport, smask, 1);     /* rising: chip samples */
                hal_delay_us(8);
                if (i >= 16 && i < 24) {        /* byte-2 window: 53 echo */
                    smp[0][i - 16] = P0; smp[1][i - 16] = P1;
                    smp[2][i - 16] = P2; smp[3][i - 16] = P3;
                }
                pin_write(sport, smask, 0);
                hal_delay_us(10);
            }
            P1 |= 0x10;                         /* VCC off */

            for (port = 0; port < 4; ++port) {
                for (bit = 0; bit < 8; ++bit) {
                    mask = (uint8_t)(1u << bit);
                    if (port == 1 && (mask & 0x12))     /* TxD2, VCC_EN */
                        continue;
                    if (port == 3 && (mask & 0x07))     /* USB, button */
                        continue;
                    if ((port == mport && mask == mmask) ||
                        (port == sport && mask == smask))
                        continue;
                    b = 0;
                    for (i = 0; i < 8; ++i)
                        b = (uint8_t)(b << 1)
                          | ((smp[port][i] & mask) ? 1 : 0);
                    if (b == 0x53) {
                        puts2(" HIT SCK="); put_pin(sport, scan_bit[si]);
                        puts2(" MISO="); put_pin(port, bit);
                        ++hits;
                    }
                }
            }
            hal_tx_byte('.');
            hal_tx_flush();
        }
        puts2("\r\n");
    }
    /* back to PINDBG idle: latches high (P1.4 high = VCC off) */
    P0 |= 0x0F; P2 = 0xFF; P3 |= 0xF8; P1 |= 0xFC;
    puts2(hits ? "SCAN DONE: see HITs\r\n" : "SCAN DONE: no sync\r\n");
}

/*
 * Clamp-decay probe: charge every candidate pin push-pull HIGH (socket
 * VCC off), snap them all to true hi-Z, then sample the ports on a
 * roughly logarithmic schedule.  A pin wired to a seated (unpowered) AVR
 * discharges through the chip's upper clamp diode into the phantom rail;
 * a floating pin/header keeps its charge for a long time.  Distinguishes
 * wired vs unwired pins — and an empty socket reads "nothing decays".
 *
 * VCC and RESET_DRV are INHERITED, not forced: with 'v+' first, a rail at
 * a real 4.2 V keeps the clamps off and clamp-class pins HOLD instead of
 * collapsing — a zero-hands "does socket VCC actually arrive" test.  Same
 * trick with 'r-' detects 12 V feeding the rail through the RESET clamp.
 */
static void
decay_probe(void)
{
    /* Per-pin fall TIME, not port snapshots: I = C*dV/dt, so with ~tens of
     * pF per node and a 500 ms window the tick counts rank per-pin leakage
     * down to ~100 pA — one leaky ESD cell among 20 pins sticks out.
     * Ticks are ~0.1 ms nominal (delay loop + port scan; UNCALIBRATED —
     * compare pins against each other or a golden run, not a datasheet).
     * t=0 fell before the first scan (load/short); '-' never fell (healthy
     * hi-Z, or held up externally like the LED line). */
    static const uint8_t dp_port[4] = { 0, 1, 2, 3 };
    static const uint8_t dp_mask[4] = { 0x0F, 0xE8, 0xFF, 0xF8 };
    static __xdata uint16_t fall[4][8];
    static __xdata uint16_t t;
    static __xdata uint8_t snap[4];
    static __xdata uint8_t k, b, busy;

    for (k = 0; k < 4; ++k)
        for (b = 0; b < 8; ++b)
            fall[k][b] = 0xFFFF;

    /* charge phase: candidates push-pull high */
    P0 |= 0x0F; P2 = 0xFF; P3 |= 0xF8; P1 |= 0xE8;
    P0M1 = 0x00; P0M0 = 0x0F;
    P2M1 = 0x00; P2M0 = 0xFF;
    P3M1 = 0x03; P3M0 = 0xF8;
    P1M0 |= 0xE8;
    hal_delay_ms(5);
    /* release phase: true hi-Z inputs */
    P0M1 = 0x0F; P0M0 = 0x00;
    P2M1 = 0xFF; P2M0 = 0x00;
    P3M1 = 0xFB; P3M0 = 0x00;
    P1M1 |= 0xE8; P1M0 &= (uint8_t)~0xE8;

    for (t = 0; t < 5000; ++t) {
        snap[0] = P0; snap[1] = P1; snap[2] = P2; snap[3] = P3;
        busy = 0;
        for (k = 0; k < 4; ++k) {
            for (b = 0; b < 8; ++b) {
                if (!(dp_mask[k] & (uint8_t)(1u << b)))
                    continue;
                if (fall[k][b] != 0xFFFF)
                    continue;
                if (snap[k] & (uint8_t)(1u << b))
                    busy = 1;
                else
                    fall[k][b] = t;
            }
        }
        if (!busy)
            break;
        hal_delay_us(150);
    }

    /* restore PINDBG quasi state, latches high (VCC stays off) */
    P0M1 = 0x00; P0M0 = 0x00;
    P1M1 = 0x00; P1M0 = 0x16;
    P2M1 = 0x00; P2M0 = 0x00;
    P3M1 = 0x03; P3M0 = 0x00;
    P0 |= 0x0F; P2 = 0xFF; P3 |= 0xF8; P1 |= 0xE8;

    puts2("fall ticks (~0.1ms, - = held):\r\n");
    for (k = 0; k < 4; ++k) {
        for (b = 0; b < 8; ++b) {
            if (!(dp_mask[k] & (uint8_t)(1u << b)))
                continue;
            put_pin(dp_port[k], b);
            hal_tx_byte('=');
            if (fall[k][b] == 0xFFFF)
                hal_tx_byte('-');
            else
                put_u16(fall[k][b]);
            puts2("\r\n");
        }
    }
    puts2("PROBE DONE\r\n");
}

/*
 * VCC_EN hunt.  Decay probing proved the chip is seated, the VCC wire and
 * ZIF contacts conduct (phantom current lights the socket LED), yet P1.2
 * high does NOT power the rail — so the real VCC_EN is likely another pin.
 * For each candidate: drive it push-pull HIGH (everything else strong low),
 * charge the DATA bus, float it, and read it back 20 ms later.  Only a
 * genuinely powered rail (clamps reverse-biased) lets P2 HOLD 0xFF; on a
 * dead rail it collapses in ~2 ms.  First line is the no-candidate control.
 */
static const uint8_t vh_port[14] = { 1, 1, 1, 1, 0, 0, 0, 0, 3, 3, 3, 3, 3, 1 };
static const uint8_t vh_bit[14]  = { 2, 3, 5, 6, 0, 1, 2, 3, 3, 4, 5, 6, 7, 7 };

static uint8_t
rail_probe(void)
{
    uint8_t v;
    P2M1 = 0x00; P2M0 = 0xFF; P2 = 0xFF;                /* charge bus */
    hal_delay_ms(2);
    P2M1 = 0xFF; P2M0 = 0x00;                            /* float it */
    hal_delay_ms(20);
    v = P2;
    return v;
}

static void
rail_report(uint8_t v)
{
    puts2(" P2="); put_hex(v);
    puts2(v == 0xFF ? "  <-- RAIL UP\r\n" : "\r\n");
    hal_tx_flush();
}

static void
vcc_hunt(void)
{
    static __xdata uint8_t c;

    /* all candidates + data bus strong low; P1.7 hi-Z until its turn
     * (UCap suspicion: don't short a possible USB-PHY regulator output).
     * NOTE (run 1 findings): a positive here just means "pin wired to the
     * chip" — one push-pull pin holds the rail through the clamp diode.
     * Only P1.2-style direct switches distinguish; treat hits as
     * connectivity data, real VCC_EN = a hit NOT wired to a socket pin. */
    P0M1 = 0x00; P0M0 = 0x0F;
    P1M1 = 0x80; P1M0 = 0x7E;
    P3M1 = 0x03; P3M0 = 0xF8;
    P0 &= (uint8_t)~0x0F; P3 &= (uint8_t)~0xF8;
    P1 &= (uint8_t)~0x6C;

    for (c = 0; c <= 14; ++c) {          /* 0 = control run, no candidate */
        if (c) {
            if (vh_bit[c - 1] == 7 && vh_port[c - 1] == 1)
                P1M1 &= (uint8_t)~0x80, P1M0 |= 0x80;   /* P1.7 push-pull */
            pin_write(vh_port[c - 1], (uint8_t)(1u << vh_bit[c - 1]), 1);
            hal_delay_ms(5);             /* switch + rail settle */
        }
        if (c) {
            puts2("cand "); put_pin(vh_port[c - 1], vh_bit[c - 1]);
        } else {
            puts2("none   ");
        }
        rail_report(rail_probe());
        if (c) {
            if (vh_bit[c - 1] == 7 && vh_port[c - 1] == 1) {
                P1M0 &= (uint8_t)~0x80; P1M1 |= 0x80;   /* P1.7 hi-Z, never
                                                           driven low */
                hal_delay_ms(2);
            } else {
                pin_write(vh_port[c - 1], (uint8_t)(1u << vh_bit[c - 1]), 0);
            }
        }
    }

    /* pins never toggled in run 1, both polarities where meaningful */
    puts2("extras:\r\n");
    P1M0 |= 0x01;                        /* P1.0 drivable */
    P3M0 |= 0x04;                        /* P3.2 drivable (button: ROM only
                                            samples it at power-on) */
    P1 |= 0x01;              hal_delay_ms(5);
    puts2("P1.0 hi"); rail_report(rail_probe());
    P1 &= (uint8_t)~0x01;    hal_delay_ms(5);
    puts2("P1.0 lo"); rail_report(rail_probe());
    P1 |= 0x01;
    P3 |= 0x04;              hal_delay_ms(5);
    puts2("P3.2 hi"); rail_report(rail_probe());
    P3 &= (uint8_t)~0x04;    hal_delay_ms(5);
    puts2("P3.2 lo"); rail_report(rail_probe());
    P3 |= 0x04;
    P1 &= (uint8_t)~0x10;    hal_delay_ms(5);
    puts2("P1.4 lo"); rail_report(rail_probe());   /* LED blips on */
    P1 |= 0x10;
    P1 |= 0x04;              hal_delay_ms(250);    /* soft-start switch? */
    puts2("P1.2 hi long"); rail_report(rail_probe());
    P1 &= (uint8_t)~0x04;

    /* restore PINDBG quasi idle */
    P0M1 = 0x00; P0M0 = 0x00;
    P1M1 = 0x00; P1M0 = 0x16;
    P2M1 = 0x00; P2M0 = 0x00;
    P3M1 = 0x03; P3M0 = 0x00;
    P0 |= 0x0F; P2 = 0xFF; P3 |= 0xF8; P1 |= 0xE8;
    P1 &= (uint8_t)~0x04;
    puts2("HUNT DONE\r\n");
}

/*
 * Read the selftest suite's USI-SPI result stream (selftest built with
 * CFG_REPORT_USI; chip powered with 'v+' and running).  Straight x61
 * wiring puts the chip's USI on our headers: USCK=PB2 -> XA1/P0.2 (we
 * master SCK), DO=PB1 -> XA0/P3.5 (MISO).  DI/PB0 ignores its input.
 *
 * Mode 0 at ~12 kHz with a ~600 us inter-byte gap (the slave reloads by
 * polling).  SCK is first held low ~20 ms so the slave's gap-resync
 * restarts its stream at the sync byte; 21 bytes still guarantee one
 * complete 10-byte frame from any phase.  Frame checks out when the
 * 8-bit sum of all 10 bytes (sync..check) is zero.
 */
static void
spi_stream_read(void)
{
    static __xdata uint8_t buf[21];
    static __xdata uint8_t i, j, s;
    uint8_t v, k;                       /* bit-bang loop stays fast */

    P0 &= (uint8_t)~0x04;               /* SCK low: settle + slave resync */
    hal_delay_ms(20);

    for (i = 0; i < 21; ++i) {
        v = 0;
        for (k = 0; k < 8; ++k) {
            v = (uint8_t)(v << 1) | ((P3 & 0x20) ? 1 : 0);
            P0 |= 0x04;
            hal_delay_us(40);
            P0 &= (uint8_t)~0x04;
            hal_delay_us(40);
        }
        buf[i] = v;
        hal_delay_us(600);              /* slave reload gap */
    }
    P0 |= 0x04;                         /* back to PINDBG idle pullup */

    puts2("raw:");
    for (i = 0; i < 21; ++i) {
        hal_tx_byte(' ');
        put_hex(buf[i]);
    }
    puts2("\r\n");
    for (i = 0; i < 10; ++i) {
        if (buf[i] != 0xA5)
            continue;
        s = 0;
        for (j = 0; j < 10; ++j)
            s = (uint8_t)(s + buf[i + j]);
        if (s)
            continue;
        puts2("frame:");
        for (j = 0; j < 10; ++j) {
            hal_tx_byte(' ');
            put_hex(buf[i + j]);
        }
        puts2(" SUM OK\r\n");
        return;
    }
    puts2("no valid frame (chip running a CFG_REPORT_USI build?)\r\n");
}

uint8_t
pindbg_rx(uint8_t b)
{
    uint8_t port, mask;

    if (!active) {
        if (!stk500v2_idle()) {
            /* Inside an STK500v2 frame: the token in programming data must
             * not drop us into pindbg mid-write.  Match inter-frame only. */
            tok_ix = 0;
            return 0;
        }
        if (b == (uint8_t)entry_token[tok_ix]) {
            if (!entry_token[++tok_ix]) {
                tok_ix = 0;
                enter();
                return 1;
            }
        } else {
            tok_ix = (b == (uint8_t)entry_token[0]) ? 1 : 0;
        }
        return 0;      /* not (yet) ours: byte also goes to STK500v2 */
    }

    if (b == '\r' || b == '\n')
        return 1;
    hal_tx_byte(b);    /* echo */

    switch (b) {
    case 'q': leave(); return 1;
    case 'p': puts2("\r\n"); print_ports(); return 1;
    case '?': puts2("\r\n"); help(); return 1;
    case 'B': puts2("\r\n"); hal_enter_isp(); return 1;
    case 'S': puts2("\r\n"); isp_scan(); return 1;
    case 'C': puts2("\r\n"); decay_probe(); return 1;
    case 'V': puts2("\r\n"); vcc_hunt(); return 1;
    case 'R': puts2("\r\n"); spi_stream_read(); return 1;
    case 'M':
        puts2(" VCC=");
        put_u16(adc_vcc_mv());
        puts2("mV guard=");
        puts2(adc_guard_enable ? "on" : "OFF");
        puts2(adc_guard_fault ? " FAULT!\r\n" : "\r\n");
        return 1;
    case 'A':
        port = pin_lookup(sel, &mask);
        adc_probe(port, mask);
        return 1;
    case 'U':
        adc_probe(1, 0x01);         /* UART2-header P1.0 pad = ADC0 */
        return 1;
    case 'W': puts2("\r\n"); wave_capture(); return 1;
    case 'T': puts2("\r\n"); tx_pattern(); return 1;
    case 'G':
        adc_guard_enable = !adc_guard_enable;
        adc_guard_fault = 0;
        puts2(adc_guard_enable ? " guard on\r\n" : " guard OFF\r\n");
        return 1;
    case '+':
    case '-':
        port = pin_lookup(sel, &mask);
        if (port == 0xFF) {
            puts2(" no pin\r\n");
        } else {
            pin_write(port, mask, b == '+');
            puts2(" ok\r\n");
        }
        return 1;
    default:
        port = pin_lookup(b, &mask);
        if (port == 0xFF) {
            puts2(" ?\r\n");
        } else {
            sel = b;
            puts2(" sel\r\n");
        }
        return 1;
    }
}
