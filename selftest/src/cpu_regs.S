/*
 * cpu_regs.S — CPU register file / SREG / SP checkerboard test.
 *
 * uint8_t cpu_regs_test(void);   returns 0 = pass, 1 = fail
 *
 * r16 is proven first with immediate-only ops (ldi/cpi), then serves as
 * the pattern donor for every other register via mov/cp.  SREG is walked
 * with 0x2A/0x55 (I bit excluded, interrupts held off throughout), SPL
 * with 0xAA/0x55 (restored immediately, nothing pushed in between), SPH
 * with its single configurable bit on a 256 B part (RAMEND=0x15F -> 0x01).
 *
 * Call-saved registers ride the stack — which the .init3 March C- has
 * already certified by the time this runs.
 */
#include <avr/io.h>
#include "config.h"

#if CFG_TEST_CPU

/* brne can't span the ~600-word test body: branch around an rjmp instead */
.macro FAILNE
    breq 1f
    rjmp cr_fail
1:
.endm

.macro TREG r
    ldi  r16, 0xAA
    mov  \r, r16
    cp   \r, r16
    FAILNE
    ldi  r16, 0x55
    mov  \r, r16
    cp   \r, r16
    FAILNE
.endm

.text
.global cpu_regs_test
cpu_regs_test:
    push r2
    push r3
    push r4
    push r5
    push r6
    push r7
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push r16
    push r17
    push r28
    push r29
    in   r18, _SFR_IO_ADDR(SREG)
    push r18
    cli

    /* r16 first, immediates only */
    ldi  r16, 0xAA
    cpi  r16, 0xAA
    FAILNE
    ldi  r16, 0x55
    cpi  r16, 0x55
    FAILNE

    TREG r0
    TREG r1
    TREG r2
    TREG r3
    TREG r4
    TREG r5
    TREG r6
    TREG r7
    TREG r8
    TREG r9
    TREG r10
    TREG r11
    TREG r12
    TREG r13
    TREG r14
    TREG r15
    TREG r17
    TREG r18
    TREG r19
    TREG r20
    TREG r21
    TREG r22
    TREG r23
    TREG r24
    TREG r25
    TREG r26
    TREG r27
    TREG r28
    TREG r29
    TREG r30
    TREG r31

    /* SREG, I bit masked out (0xAA&0x7F=0x2A, 0x55&0x7F=0x55) */
    ldi  r16, 0x2A
    out  _SFR_IO_ADDR(SREG), r16
    in   r17, _SFR_IO_ADDR(SREG)
    cp   r17, r16
    FAILNE
    ldi  r16, 0x55
    out  _SFR_IO_ADDR(SREG), r16
    in   r17, _SFR_IO_ADDR(SREG)
    cp   r17, r16
    FAILNE
    clr  r17
    out  _SFR_IO_ADDR(SREG), r17

    /* SPL walk — restore before any stack use */
    in   r18, _SFR_IO_ADDR(SPL)
    ldi  r16, 0xAA
    out  _SFR_IO_ADDR(SPL), r16
    in   r17, _SFR_IO_ADDR(SPL)
    out  _SFR_IO_ADDR(SPL), r18
    cp   r17, r16
    FAILNE
    ldi  r16, 0x55
    out  _SFR_IO_ADDR(SPL), r16
    in   r17, _SFR_IO_ADDR(SPL)
    out  _SFR_IO_ADDR(SPL), r18
    cp   r17, r16
    FAILNE

    /* SPH: only bit 0 is implemented with 256 B SRAM */
    in   r19, _SFR_IO_ADDR(SPH)
    ldi  r16, 0x01
    out  _SFR_IO_ADDR(SPH), r16
    in   r17, _SFR_IO_ADDR(SPH)
    out  _SFR_IO_ADDR(SPH), r19
    andi r17, 0x01
    cp   r17, r16
    FAILNE
    ldi  r16, 0x00
    out  _SFR_IO_ADDR(SPH), r16
    in   r17, _SFR_IO_ADDR(SPH)
    out  _SFR_IO_ADDR(SPH), r19
    andi r17, 0x01
    cp   r17, r16
    FAILNE

    ldi  r24, 0
    rjmp cr_out
cr_fail:
    ldi  r24, 1
cr_out:
    pop  r18
    out  _SFR_IO_ADDR(SREG), r18
    pop  r29
    pop  r28
    pop  r17
    pop  r16
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  r7
    pop  r6
    pop  r5
    pop  r4
    pop  r3
    pop  r2
    clr  r1
    ret

#endif /* CFG_TEST_CPU */
