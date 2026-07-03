/*
 * Board pin map — STK600-clone 4-in-1 HVPP programmer, STC8H8K64U LQFP32.
 * Single source of truth; traced 2026-07-02 (PLAN.md "Trace results").
 *
 * DATA0..7 = P2.0..P2.7 (byte-aligned: P2 = data bus, one operation).
 * UART2 header: P1.0 = RxD2, P1.1 = TxD2 (peripheral-fixed).
 * USB: P3.0 = D-, P3.1 = D+ (reserved for later CDC transport).
 * P3.2 = bootloader button (ROM meaning at power-on only).
 */

#ifndef BOARD_H
#define BOARD_H

#include "stc8h.h"

/*
 * P1 block RESOLVED 2026-07-02 late: the STC8H datasheet holds MULTIPLE
 * different LQFP32 pinouts (chip variants) and the original name mapping
 * used the wrong variant's page.  True STC8H8K64U LQFP32 row: pins 1-8 =
 * P1.0, P1.1, P1.4, P1.5, P1.6, P1.7, P1.3, UCap — there is NO P1.2 pad.
 * With the correct row the ORIGINAL PHYSICAL TRACE was right on every pin
 * and coincides exactly with the clamp/ISP-scan forensic proofs.
 *   pin 4 = P1.5 = ISP/JTAG header RST via 100R (NOT on the HVPP ZIF)
 *   pin 5 = P1.6 = red LED via 1k to 4.2 V, low = on (= PIN_LED/dummy BS2)
 *   pin 8 = UCap (USB LDO cap — the "mystery capacitor"); P1.2 register
 *           bits are pad-less and inert on this package.
 */
#define PIN_XA0        P3_5    /* PROVEN 2026-07-02: ISP-scan MISO hit */
#define PIN_XA1        P0_2    /* PROVEN 2026-07-02: ISP-scan SCK hit */
#define PIN_BS1        P3_6    /* clamp-coupled to chip (wired) + table */
#define PIN_BS2        P1_6    /* DUMMY = the red LED line (1k to 4.2 V).
                                  Real BS2 header pin unknown; x61 remap
                                  never needs it (BS2 rides the XA1 line)
                                  and hal_ctrl_set writes BS2 before WR, so
                                  it must NOT alias a real control pin.
                                  Side effect: red LED blinks with BS2
                                  stack activity — a free activity light. */
#define PIN_OE         P1_7    /* PROVEN 2026-07-02: HVPP sig+fuse reads
                                  correct with this assignment */
#define PIN_WR         P1_3    /* PROVEN 2026-07-02: ISP-scan MOSI hit
                                  (ex-"P1.6" was the datasheet-variant
                                  mix-up; the traced package pin was right) */
#define PIN_PAGEL      P0_3    /* pin 32, name table-confirmed */
#define PIN_XTAL1      P0_0    /* pin 29, board silk "CLK", table-confirmed */
#define PIN_RDY        P0_1    /* pin 30, input RDY/BSY, table-confirmed */
#define PIN_VCC_EN     P1_4    /* CONFIRMED 2026-07-02 (clamp forensics +
                                  bench): ACTIVE LOW high-side switch, PNP
                                  2TY with 1k base resistor.  Quasi
                                  boot-high latch = socket OFF at power-on:
                                  inherently fail-safe. */
#define PIN_RESET_DRV  P5_4    /* see RESET_DRV_GND_LEVEL below */
#define PIN_LED        P1_6    /* the RED LED: 1k to the 4.2 V rail, low =
                                  on (bench-confirmed 2026-07-02).  Shared
                                  with dummy BS2, so it flickers during
                                  HVPP sessions.  The GREEN LED is not on
                                  an MCU pin (lives in the switched-VCC
                                  network under the ZIFs, exact topology
                                  still unknown). */

/*
 * Polarity parameters — all three confirmed on the bench 2026-07-02.
 *
 * RESET_DRV_GND_LEVEL: NPN base is fed from 4.2 V via 1k, with P5.4 hanging
 * below through a second 1k.  Default (P5.4 high or hi-Z) the NPN saturates
 * and clamps the RESET node to GND — measured 2.5 V at P5.4 with stock
 * firmware is exactly Vbe + 1k * (4.2-0.7)/2k = the hi-Z signature.
 * Driving P5.4 LOW sinks the ~1.75 mA base current, NPN turns off, RESET
 * flies to 12 V through its 1k pull-up.  Fail-safe: 12 V needs a deliberate
 * low, boot state keeps the target in reset.
 * CAVEAT 2026-07-02: only the GND side is bench-confirmed (idle levels).
 * The LOW->12V direction is inferred, and simple divider math on the
 * described topology says a strong low might NOT starve the base (2.8 mA
 * still flows in) -- there may be a second driver stage.  If PINDBG 'r-'
 * does not put 12 V on the RESET header, this polarity (or topology) is
 * the HVPP no-progmode-entry bug.
 *
 * VCC_ON_LEVEL: active LOW (PNP high-side switch; forensics + bench
 * confirmed).  Quasi-bidir latches boot high = socket OFF from power-on:
 * fail-safe with no ordering constraints.
 */
#define RESET_DRV_GND_LEVEL  1    /* confirmed (12 V on low: remote-verified
                                     2026-07-02, control-pin net energized) */
#define VCC_ON_LEVEL         0    /* confirmed: P1.4 low = rail up (clamp
                                     decay probe holds 4.2 V) */
#define LED_ON_LEVEL         0    /* red LED: 1k to 4.2 V, low = on */

/*
 * GPIO mode init (PxM1:PxM0 — 00 quasi, 01 push-pull, 10 input hi-Z).
 *  P0: 0=XTAL1 out, 1=RDY in, 2=XA1 out, 3=PAGEL out
 *  P1: 1=TxD2 push-pull; 3=WR, 4=VCC_EN, 6=LED/BS2 out; 0=RxD2 quasi;
 *      5=ISP-header RST left quasi (weak high releases an ISP target);
 *      7=OE quasi (strong low is what matters); bit2 pad-less (UCap), inert
 *  P2: data bus, starts driven
 *  P3: 5=XA0, 6=BS1 out; 0/1 = USB D-/D+ kept hi-Z (PHY owns them;
 *      never drive — push-pull high on D+ fakes a USB attach), 2=button
 *  P5: 4=RESET_DRV out
 * Caller must clear the VCC_EN (and data-bus) latches BEFORE invoking this:
 * the boot-high latch would otherwise be driven strongly onto an active-high
 * VCC switch.  RESET is fine either way (boot-high = held at GND).
 */
#define BOARD_GPIO_INIT() do {              \
        P0M1 = 0x02; P0M0 = 0x0D;           \
        P1M1 = 0x00; P1M0 = 0x5A;           \
        P2M1 = 0x00; P2M0 = 0xFF;           \
        P3M1 = 0x03; P3M0 = 0x60;           \
        P5M1 = 0x00; P5M0 = 0x10;           \
    } while (0)

#define BOARD_DATA_OUT()   do { P2M1 = 0x00; P2M0 = 0xFF; } while (0)
#define BOARD_DATA_IN()    do { P2M1 = 0xFF; P2M0 = 0x00; } while (0)

/* Control pins back to quasi-bidir inputs (leave progmode / released idle);
 * TxD2 (P1.1) and VCC_EN (P1.4) stay push-pull, XTAL1 (P0.0) stays
 * push-pull with its latch parked LOW, USB D-/D+ stay hi-Z.  Red LED
 * (P1.6) goes quasi with latch high = off. */
#define BOARD_CTRL_RELEASE() do {           \
        P0M1 = 0x02; P0M0 = 0x01;           \
        P1M1 = 0x00; P1M0 = 0x12;           \
        P3M1 = 0x03; P3M0 = 0x00;           \
    } while (0)

/* Counterpart: full push-pull drive for an active progmode session
 * (P2 data bus via BOARD_DATA_OUT/IN, P5 never released). */
#define BOARD_CTRL_ENGAGE() do {            \
        P0M1 = 0x02; P0M0 = 0x0D;           \
        P1M1 = 0x00; P1M0 = 0x5A;           \
        P3M1 = 0x03; P3M0 = 0x60;           \
    } while (0)

#define FOSC 24000000UL    /* IRC, matches shipped option bytes; USB needs it */

#endif
