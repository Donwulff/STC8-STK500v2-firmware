/*
 * march.S — March C- over the ENTIRE SRAM (0x60..RAMEND) at .init3.
 *
 * Runs before .data copy / .bss clear, with an empty stack and SP already
 * set (.init2), so the whole 256-byte array is testable in one pass with
 * no save/restore gymnastics.  Registers only; zero RAM use.
 *
 * Solid 0x00/0xFF patterns, classic six elements:
 *   M0 up(w0)  M1 up(r0,w1)  M2 up(r1,w0)  M3 dn(r0,w1)  M4 dn(r1,w0)  M5 up(r0)
 * Covers stuck-at, transition and (address-decoder) coupling faults.
 * Algorithm: van de Goor, "Testing Semiconductor Memories" — original
 * implementation (the Microchip Class B library is reference-only, see
 * host/fetch_classb_ref.sh).
 *
 * Result posted in GPIOR0 (I/O reg, survives the test, readable from C):
 *   0x2A pass, 0x2F fail, 0x00 = not run (skipped or feature off).
 *
 * SKIPPED when MCUSR.WDRF is set: a watchdog reset is part of the WDT test
 * and the first-boot results ride in .noinit — marching over them would
 * destroy the evidence.  MCUSR itself is left untouched for main().
 */
#include <avr/io.h>
#include "config.h"

#if CFG_TEST_SRAM

/* ascending element: expect \rd in every cell, replace with \wr */
.macro ASCEL rd, wr
    ldi  r26, lo8(RAMSTART)
    ldi  r27, hi8(RAMSTART)
0:  ld   r22, X
    cp   r22, \rd
    brne march_fail
    st   X+, \wr
    cpi  r26, lo8(RAMEND+1)
    cpc  r27, r18
    brne 0b
.endm

/* descending element */
.macro DSCEL rd, wr
    ldi  r26, lo8(RAMEND+1)
    ldi  r27, hi8(RAMEND+1)
0:  ld   r22, -X
    cp   r22, \rd
    brne march_fail
    st   X, \wr
    cpi  r26, lo8(RAMSTART)
    cpc  r27, r19
    brne 0b
.endm

.section .init3,"ax",@progbits
    in   r24, _SFR_IO_ADDR(MCUSR)
    sbrc r24, WDRF
    rjmp march_done

    ldi  r18, hi8(RAMEND+1)
    ldi  r19, hi8(RAMSTART)
    clr  r20                    /* "0" pattern */
    ldi  r21, 0xFF              /* "1" pattern */

    /* M0: up, write 0 (no read) */
    ldi  r26, lo8(RAMSTART)
    ldi  r27, hi8(RAMSTART)
0:  st   X+, r20
    cpi  r26, lo8(RAMEND+1)
    cpc  r27, r18
    brne 0b

    ASCEL r20, r21              /* M1: up   r0 w1 */
    ASCEL r21, r20              /* M2: up   r1 w0 */
    DSCEL r20, r21              /* M3: down r0 w1 */
    DSCEL r21, r20              /* M4: down r1 w0 */
    ASCEL r20, r20              /* M5: up   r0 (write-back 0) */

    ldi  r24, 0x2A
    out  _SFR_IO_ADDR(GPIOR0), r24
    rjmp march_done
march_fail:
    ldi  r24, 0x2F
    out  _SFR_IO_ADDR(GPIOR0), r24
march_done:

#endif /* CFG_TEST_SRAM */
