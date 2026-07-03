/*
 * USB CDC-ACM transport for the STC8H8K64U USB device controller.
 * Ported from STC demo #61 "CDC协议范例" (Keil C51) to SDCC, stripped to
 * what a virtual COM port needs: EP0 control, EP1 bulk IN/OUT data,
 * EP2 interrupt IN (CDC notification element, never used but declared).
 *
 * PORTING NOTE: Keil C51 is big-endian for multi-byte types, SDCC mcs51 is
 * little-endian.  The demo's reverse2()/reverse4() calls existed only to fix
 * Keil-endianness against USB's little-endian wire format — under SDCC the
 * wire order IS the memory order, so they are deliberately absent here.
 *
 * Concurrency model (same as the demo): the ISR owns all USB registers;
 * main-line code (tx pump) masks the USB interrupt (IE2 bit7 EUSB) around
 * any register access.  The indexed-register file (INDEX + CSRs) makes this
 * mandatory, not just polite.
 *
 * LineCoding (baud/format) is accepted and stored but ignored: this
 * transport is virtual, there is no physical serial behind it.
 *
 * PROVENANCE: STC's demo code ships with no license text; this file is
 * therefore NOT covered by the repository's BSD license — see
 * THIRD_PARTY_NOTICES.md.  The usb_cdc.h interface is small; any CDC-ACM
 * stack for the STC8H USB device core can replace this file behind it.
 */

#include "stc8h.h"
#include "usb_cdc.h"

/* ---- USB controller register file (via USBADR/USBDAT window) ---- */
#define R_FADDR     0
#define R_POWER     1
#define R_INTRIN1   2
#define R_INTROUT1  4
#define R_INTRUSB   6
#define R_INTRIN1E  7
#define R_INTROUT1E 9
#define R_INTRUSBE  11
#define R_INDEX     14
#define R_INMAXP    16
#define R_CSR0      17
#define R_INCSR1    17
#define R_INCSR2    18
#define R_OUTMAXP   19
#define R_OUTCSR1   20
#define R_OUTCSR2   21
#define R_COUNT0    22
#define R_FIFO0     32
#define R_FIFO1     33
#define R_FIFO2     34

/* CSR0 bits */
#define SSUEND      0x80
#define SOPRDY      0x40
#define SDSTL       0x20
#define SUEND       0x10
#define DATEND      0x08
#define STSTL       0x04
#define IPRDY       0x02
#define OPRDY       0x01
/* INCSR1 bits */
#define INCLRDT     0x40
#define INSTSTL     0x20
#define INSDSTL     0x10
#define INFLUSH     0x08
#define INUNDRUN    0x04
#define INIPRDY     0x01
/* INCSR2 bits */
#define INMODEIN    0x20
/* OUTCSR1 bits */
#define OUTCLRDT    0x80
#define OUTSTSTL    0x40
#define OUTSDSTL    0x20
#define OUTFLUSH    0x10
#define OUTOPRDY    0x01
/* INTRUSB bits */
#define RSTIF       0x04
#define RSUIF       0x02
#define SUSIF       0x01

#define EP0_SIZE    64
#define EP1_SIZE    64

/* setup packet layout (wire order == SDCC memory order, no padding) */
typedef struct {
    uint8_t  bmRequestType, bRequest;
    uint8_t  wValueL, wValueH;
    uint8_t  wIndexL, wIndexH;
    uint16_t wLength;
} SETUP;

enum { EP_IDLE, EP_DATAIN, EP_DATAOUT, EP_STALL };
enum { DEV_DEFAULT, DEV_ADDRESS, DEV_CONFIGURED };

static SETUP setup;
static uint8_t dev_state;
static uint8_t ep0_state;
static uint16_t ep0_size;
static uint8_t *ep0_ptr;
static uint8_t in_halt, out_halt;   /* EP halt bitmaps for GET_STATUS */

static volatile __bit in_busy;
static volatile __bit out_stalled;  /* OUT unarmed: rx ring was near-full */

/* rx = host->us (OUT EP1), tx = us->host (IN EP1); 8-bit wrap = size 256 */
static volatile uint8_t rx_w, rx_r, tx_w, tx_r;
static __xdata uint8_t rx_ring[256];
static __xdata uint8_t tx_ring[256];

static __xdata uint8_t line_coding[7] = {
    0x00, 0xC2, 0x01, 0x00,     /* 115200 little-endian */
    0, 0, 8                     /* 1 stop, no parity, 8 data */
};

/* ---- descriptors ---- */

static __code uint8_t DEVICEDESC[18] = {
    18, 0x01, 0x00, 0x02,       /* USB 2.00 */
    0x02, 0x00, 0x00, 64,       /* CDC class, EP0 64 */
    0xBF, 0x34, 0x02, 0xFF,     /* 34bf:ff02 (STC CDC example IDs) */
    0x00, 0x01,                 /* bcdDevice 1.00 */
    1, 2, 0, 1
};

static __code uint8_t CONFIGDESC[67] = {
    9, 0x02, 67, 0, 2, 1, 0, 0x80, 50,          /* bus-powered, 100 mA */
    /* interface 0: CDC control */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 0x01,                  /* call mgmt */
    4, 0x24, 0x02, 0x02,                        /* ACM caps */
    5, 0x24, 0x06, 0x00, 0x01,                  /* union 0/1 */
    7, 0x05, 0x82, 0x03, 64, 0, 255,            /* EP2 IN intr (notif) */
    /* interface 1: CDC data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, 0x81, 0x02, 64, 0, 0,              /* EP1 IN bulk */
    7, 0x05, 0x01, 0x02, 64, 0, 0               /* EP1 OUT bulk */
};

static __code uint8_t LANGIDDESC[4] = { 4, 0x03, 0x09, 0x04 };
static __code uint8_t MANUFACTDESC[10] = {
    10, 0x03, 'D',0, 'I',0, 'Y',0, '-',0
};
static __code uint8_t PRODUCTDESC[22] = {
    22, 0x03, 'A',0,'V',0,'R',0,' ',0,'H',0,'V',0,'P',0,'P',0,' ',0,'P',0
};
static __code uint8_t PACKET0[2] = { 0, 0 };
static __code uint8_t PACKET1[2] = { 1, 0 };

/* ---- register window ---- */

static uint8_t
ureg_read(uint8_t addr)
{
    while (USBADR & 0x80)
        ;
    USBADR = addr | 0x80;
    while (USBADR & 0x80)
        ;
    return USBDAT;
}

/* __reentrant is load-bearing: ureg_write is called from BOTH the USB ISR
 * and mainline.  Without it SDCC parks the 2nd parameter in the shared
 * OSEG overlay slot (0x71-ish) — the SAME address the linker gives
 * __moduint/__divuint/__mulint/__memcpy parameters — so an EP1-IN underrun
 * interrupt (host polls IN at ~kHz whenever the port is open) landing
 * inside any mainline division corrupted the divisor.  Bench signature:
 * put_u16 emitting '0'+garbage chars ('<', ':') and 5-digit values in
 * dense PINDBG dumps, while put_hex (table lookup, no division) stayed
 * clean.  Found 2026-07-03 via the 'T' deterministic TX pattern.
 * ureg_read is single-param (register-passed) = safe as-is, but any future
 * ISR-reachable function with 2+ byte params needs this same treatment. */
static void
ureg_write(uint8_t addr, uint8_t dat) __reentrant
{
    while (USBADR & 0x80)
        ;
    USBADR = addr & 0x7F;
    USBDAT = dat;
}

/* ---- EP0 helpers ---- */

static void
ep0_stall(void)
{
    ep0_state = EP_STALL;
    ureg_write(R_CSR0, SOPRDY | SDSTL);
}

static void
ep0_status(void)
{
    ep0_state = EP_IDLE;
    ureg_write(R_CSR0, SOPRDY | DATEND);
}

static void
ep0_send(void)
{
    uint8_t csr, cnt;

    ureg_write(R_INDEX, 0);
    csr = ureg_read(R_CSR0);
    if (csr & IPRDY)
        return;
    cnt = ep0_size > EP0_SIZE ? EP0_SIZE : (uint8_t)ep0_size;
    ep0_size -= cnt;
    while (cnt--)
        ureg_write(R_FIFO0, *ep0_ptr++);
    if (ep0_size == 0) {
        ureg_write(R_CSR0, IPRDY | DATEND);
        ep0_state = EP_IDLE;
    } else {
        ureg_write(R_CSR0, IPRDY);
    }
}

static void
ep0_start_in(void)
{
    if (ep0_size > setup.wLength)
        ep0_size = setup.wLength;
    ep0_state = EP_DATAIN;
    ureg_write(R_CSR0, SOPRDY);
    ep0_send();
}

static void
ep0_recv(void)
{
    uint8_t csr, cnt;

    ureg_write(R_INDEX, 0);
    csr = ureg_read(R_CSR0);
    if (!(csr & OPRDY))
        return;
    cnt = ureg_read(R_COUNT0);
    while (cnt--) {
        uint8_t b = ureg_read(R_FIFO0);
        if (ep0_size) {
            *ep0_ptr++ = b;
            ep0_size--;
        }
    }
    if (ep0_size == 0) {
        ureg_write(R_CSR0, SOPRDY | DATEND);
        ep0_state = EP_IDLE;
    } else {
        ureg_write(R_CSR0, SOPRDY);
    }
    /* SET_LINE_CODING payload arrived: stored, deliberately not applied */
}

/* ---- standard requests ---- */

static void
req_get_descriptor(void)
{
    switch (setup.wValueH) {
    case 0x01: ep0_ptr = (uint8_t *)DEVICEDESC; ep0_size = sizeof(DEVICEDESC); break;
    case 0x02: ep0_ptr = (uint8_t *)CONFIGDESC; ep0_size = sizeof(CONFIGDESC); break;
    case 0x03:
        switch (setup.wValueL) {
        case 0: ep0_ptr = (uint8_t *)LANGIDDESC;   ep0_size = sizeof(LANGIDDESC);   break;
        case 1: ep0_ptr = (uint8_t *)MANUFACTDESC; ep0_size = sizeof(MANUFACTDESC); break;
        case 2: ep0_ptr = (uint8_t *)PRODUCTDESC;  ep0_size = sizeof(PRODUCTDESC);  break;
        default: ep0_stall(); return;
        }
        break;
    default: ep0_stall(); return;
    }
    ep0_start_in();
}

static void
req_set_configuration(void)
{
    if (setup.wValueL == 1) {
        dev_state = DEV_CONFIGURED;
        in_halt = 0;
        out_halt = 0;
        ureg_write(R_INDEX, 1);
        ureg_write(R_INCSR2, INMODEIN);         /* EP1 = IN side config */
        ureg_write(R_INMAXP, EP1_SIZE / 8);
        ureg_write(R_INCSR2, 0);                /* EP1 = OUT side config */
        ureg_write(R_OUTMAXP, EP1_SIZE / 8);
        ureg_write(R_INDEX, 2);
        ureg_write(R_INCSR2, INMODEIN);         /* EP2 IN (notification) */
        ureg_write(R_INMAXP, EP1_SIZE / 8);
        ureg_write(R_INDEX, 0);
    } else {
        dev_state = DEV_ADDRESS;
    }
    ep0_status();
}

static void
req_std(void)
{
    uint8_t ep;

    switch (setup.bRequest) {
    case 0x00:                                  /* GET_STATUS */
        ep = setup.wIndexL & 0x0F;
        if ((setup.bmRequestType & 0x0F) == 0x02 &&
            ((setup.wIndexL & 0x80) ? (in_halt & (1 << ep))
                                    : (out_halt & (1 << ep))))
            ep0_ptr = (uint8_t *)PACKET1;
        else
            ep0_ptr = (uint8_t *)PACKET0;
        ep0_size = 2;
        ep0_start_in();
        break;
    case 0x01:                                  /* CLEAR_FEATURE (EP halt) */
    case 0x03:                                  /* SET_FEATURE  (EP halt) */
        ep = setup.wIndexL & 0x0F;
        if ((setup.bmRequestType & 0x0F) != 0x02 || ep == 0 || ep > 2) {
            ep0_stall();
            return;
        }
        ureg_write(R_INDEX, ep);
        if (setup.wIndexL & 0x80) {             /* IN endpoint */
            if (setup.bRequest == 0x03) {
                in_halt |= (uint8_t)(1 << ep);
                ureg_write(R_INCSR1, INSDSTL);
            } else {
                in_halt &= (uint8_t)~(1 << ep);
                ureg_write(R_INCSR1, INCLRDT);
            }
        } else {
            if (setup.bRequest == 0x03) {
                out_halt |= (uint8_t)(1 << ep);
                ureg_write(R_OUTCSR1, OUTSDSTL);
            } else {
                out_halt &= (uint8_t)~(1 << ep);
                ureg_write(R_OUTCSR1, OUTCLRDT);
            }
        }
        ureg_write(R_INDEX, 0);
        ep0_status();
        break;
    case 0x05:                                  /* SET_ADDRESS */
        ureg_write(R_FADDR, setup.wValueL);
        dev_state = setup.wValueL ? DEV_ADDRESS : DEV_DEFAULT;
        ep0_status();
        break;
    case 0x06:                                  /* GET_DESCRIPTOR */
        req_get_descriptor();
        break;
    case 0x08:                                  /* GET_CONFIGURATION */
        ep0_ptr = (uint8_t *)(dev_state == DEV_CONFIGURED ? PACKET1 : PACKET0);
        ep0_size = 1;
        ep0_start_in();
        break;
    case 0x09:                                  /* SET_CONFIGURATION */
        req_set_configuration();
        break;
    case 0x0A:                                  /* GET_INTERFACE */
        ep0_ptr = (uint8_t *)PACKET0;
        ep0_size = 1;
        ep0_start_in();
        break;
    case 0x0B:                                  /* SET_INTERFACE */
        ep0_status();
        break;
    default:
        ep0_stall();
        break;
    }
}

/* ---- CDC class requests ---- */

static void
req_class(void)
{
    switch (setup.bRequest) {
    case 0x20:                                  /* SET_LINE_CODING */
        ep0_ptr = line_coding;
        ep0_size = setup.wLength > 7 ? 7 : setup.wLength;
        ep0_state = EP_DATAOUT;
        ureg_write(R_CSR0, SOPRDY);
        break;
    case 0x21:                                  /* GET_LINE_CODING */
        ep0_ptr = line_coding;
        ep0_size = 7;
        ep0_start_in();
        break;
    case 0x22:                                  /* SET_CONTROL_LINE_STATE */
        ep0_status();
        break;
    default:
        ep0_stall();
        break;
    }
}

/* ---- ISR-side endpoint events ---- */

static void
ev_setup(void)
{
    uint8_t csr, cnt;
    uint8_t *p;

    ureg_write(R_INDEX, 0);
    csr = ureg_read(R_CSR0);

    if (csr & STSTL) {
        ureg_write(R_CSR0, csr & (uint8_t)~STSTL);
        ep0_state = EP_IDLE;
    }
    if (csr & SUEND)
        ureg_write(R_CSR0, csr | SSUEND);

    switch (ep0_state) {
    case EP_IDLE:
        if (csr & OPRDY) {
            cnt = ureg_read(R_COUNT0);
            p = (uint8_t *)&setup;
            while (cnt--)
                *p++ = ureg_read(R_FIFO0);
            switch (setup.bmRequestType & 0x60) {
            case 0x00: req_std();   break;
            case 0x20: req_class(); break;
            default:   ep0_stall(); break;
            }
        }
        break;
    case EP_DATAIN:
        ep0_send();
        break;
    case EP_DATAOUT:
        ep0_recv();
        break;
    default:
        break;
    }
}

static void
ev_reset(void)
{
    ureg_write(R_FADDR, 0);
    dev_state = DEV_DEFAULT;
    ep0_state = EP_IDLE;
    ureg_write(R_INDEX, 1);
    ureg_write(R_INCSR1, INCLRDT | INFLUSH);
    ureg_write(R_OUTCSR1, OUTCLRDT | OUTFLUSH);
    ureg_write(R_INDEX, 2);
    ureg_write(R_INCSR1, INCLRDT | INFLUSH);
    ureg_write(R_INDEX, 0);
    /* forget any half-done traffic so a replug can't deadlock the pump */
    in_busy = 0;
    out_stalled = 0;
    rx_w = rx_r = tx_w = tx_r = 0;
}

static void
ev_in_ep1(void)
{
    /* RACE (bench-caught 2026-07-03 as duplicated/dropped chars in dense
     * PINDBG output): this interrupt may be the completion of packet N-1
     * arriving AFTER the mainline pump already armed packet N.  The vendor
     * demo unconditionally clears its busy flag here and writes 0 to CSR on
     * underrun — both clobber the freshly-armed packet (dup + drop).  Only
     * declare the FIFO free when INIPRDY really is clear, and preserve an
     * armed packet when clearing the underrun flag. */
    uint8_t csr;

    ureg_write(R_INDEX, 1);
    csr = ureg_read(R_INCSR1);
    if (csr & INSTSTL)
        ureg_write(R_INCSR1, (uint8_t)(INCLRDT | (csr & INIPRDY)));
    if (csr & INUNDRUN)
        ureg_write(R_INCSR1, (uint8_t)(csr & INIPRDY));
    if (!(csr & INIPRDY))
        in_busy = 0;
}

static void
ev_in_ep2(void)
{
    uint8_t csr;

    ureg_write(R_INDEX, 2);
    csr = ureg_read(R_INCSR1);
    if (csr & INSTSTL)
        ureg_write(R_INCSR1, INCLRDT);
    if (csr & INUNDRUN)
        ureg_write(R_INCSR1, 0);
}

static void
ev_out_ep1(void)
{
    uint8_t csr, cnt;

    ureg_write(R_INDEX, 1);
    csr = ureg_read(R_OUTCSR1);
    if (csr & OUTSTSTL)
        ureg_write(R_OUTCSR1, OUTCLRDT);
    if (csr & OUTOPRDY) {
        cnt = ureg_read(R_COUNT0);      /* OUTCOUNT1 shares the address */
        while (cnt--) {
            rx_ring[rx_w] = ureg_read(R_FIFO1);
            ++rx_w;
        }
        if ((uint8_t)(rx_w - rx_r) >= (uint8_t)(256 - EP1_SIZE)) {
            out_stalled = 1;            /* re-armed by usb_cdc_poll() */
        } else {
            ureg_write(R_OUTCSR1, 0);
        }
    }
}

void
usb_isr(void) __interrupt(25)
{
    uint8_t iusb, iin, iout;

    iusb = ureg_read(R_INTRUSB);
    iin  = ureg_read(R_INTRIN1);
    iout = ureg_read(R_INTROUT1);

    if (iusb & RSTIF)
        ev_reset();
    if (iin & 0x01)
        ev_setup();
    if (iin & 0x02)
        ev_in_ep1();
    if (iin & 0x04)
        ev_in_ep2();
    if (iout & 0x02)
        ev_out_ep1();
    /* suspend/resume: nothing to do, bus-powered device */
}

/* ---- public API (main-line side; masks EUSB around register access) ---- */

#define EUSB_OFF()  (IE2 &= (uint8_t)~0x80)
#define EUSB_ON()   (IE2 |= 0x80)

void
usb_cdc_init(void)
{
    /* D-/D+ = P3.0/P3.1 to hi-Z input */
    P3M0 &= (uint8_t)~0x03;
    P3M1 |= 0x03;

    P_SW2 |= 0x80;                  /* EAXFR for IRC48MCR */
    IRC48MCR = 0x80;                /* start the 48 MHz USB oscillator */
    while (!(IRC48MCR & 0x01))
        ;

    USBCLK = 0x00;                  /* 48 MHz / 1 */
    USBCON = 0x90;                  /* enable PHY + DP pull-up */

    ureg_write(R_FADDR, 0x00);
    ureg_write(R_POWER, 0x08);      /* reset the controller */
    ureg_write(R_INTRIN1E, 0x3F);
    ureg_write(R_INTROUT1E, 0x3F);
    ureg_write(R_INTRUSBE, 0x07);
    ureg_write(R_POWER, 0x00);

    dev_state = DEV_DEFAULT;
    ep0_state = EP_IDLE;
    in_busy = 0;
    out_stalled = 0;
    rx_w = rx_r = tx_w = tx_r = 0;

    IE2 |= 0x80;                    /* EUSB */
}

uint8_t
usb_cdc_configured(void)
{
    return dev_state == DEV_CONFIGURED;
}

uint8_t
usb_cdc_rx_ready(void)
{
    return rx_w != rx_r;
}

uint8_t
usb_cdc_rx_get(void)
{
    uint8_t b = rx_ring[rx_r];
    ++rx_r;
    return b;
}

void
usb_cdc_poll(void)
{
    uint8_t cnt;

    if (dev_state != DEV_CONFIGURED)
        return;

    if (!in_busy && tx_r != tx_w) {
        EUSB_OFF();
        ureg_write(R_INDEX, 1);
        /* Hardware truth beats the flag: never load while a packet is
         * still armed (see ev_in_ep1 race note), retry on the next poll. */
        if (!(ureg_read(R_INCSR1) & INIPRDY)) {
            in_busy = 1;
            cnt = 0;
            while (tx_r != tx_w && cnt < EP1_SIZE) {
                ureg_write(R_FIFO1, tx_ring[tx_r]);
                ++tx_r;
                ++cnt;
            }
            ureg_write(R_INCSR1, INIPRDY);
        }
        EUSB_ON();
    }

    if (out_stalled && (uint8_t)(rx_w - rx_r) < (uint8_t)(256 - EP1_SIZE)) {
        EUSB_OFF();
        out_stalled = 0;
        ureg_write(R_INDEX, 1);
        ureg_write(R_OUTCSR1, 0);   /* re-arm OUT */
        EUSB_ON();
    }
}

void
usb_cdc_tx(uint8_t b)
{
    /* Ring full: pump until the host drains a packet — but bounded.  A
     * surprise unplug gives no reset event (no VBUS sense), so an unbounded
     * wait would hang the firmware; dropping the backlog on a wedged host
     * beats needing the recovery button. */
    uint16_t guard = 50000;
    while ((uint8_t)(tx_w - tx_r) == 255) {
        usb_cdc_poll();
        if (--guard == 0) {
            tx_r = tx_w;
            break;
        }
    }
    tx_ring[tx_w] = b;
    ++tx_w;
}
