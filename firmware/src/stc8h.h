/*
 * Minimal STC8H8K64U register definitions for SDCC — only what this firmware
 * uses.  Addresses verified against STC's Keil header (demo COMM/STC8H.h).
 */

#ifndef STC8H_H
#define STC8H_H

#ifndef __SDCC
#error "stc8h.h is target-only; host builds must not include it"
#endif

__sfr __at(0x80) P0;
__sfr __at(0x90) P1;
__sfr __at(0xA0) P2;
__sfr __at(0xB0) P3;
__sfr __at(0xC8) P5;

/* Pin modes: M1=0,M0=0 quasi-bidir; 0,1 push-pull; 1,0 input hi-Z; 1,1 open-drain */
__sfr __at(0x93) P0M1;
__sfr __at(0x94) P0M0;
__sfr __at(0x91) P1M1;
__sfr __at(0x92) P1M0;
__sfr __at(0x95) P2M1;
__sfr __at(0x96) P2M0;
__sfr __at(0xB1) P3M1;
__sfr __at(0xB2) P3M0;
__sfr __at(0xC9) P5M1;
__sfr __at(0xCA) P5M0;

__sfr __at(0xA8) IE;
__sfr __at(0xAF) IE2;       /* bit0 = ES2 (UART2 interrupt enable) */
__sfr __at(0x8E) AUXR;      /* bit4 T2R, bit3 T2_C/T, bit2 T2x12 */
__sfr __at(0x9A) S2CON;     /* bit7 S2SM0, bit4 S2REN, bit1 S2TI, bit0 S2RI */
__sfr __at(0x9B) S2BUF;
__sfr __at(0xD6) T2H;
__sfr __at(0xD7) T2L;
__sfr __at(0xBA) P_SW2;     /* bit7 EAXFR: XFR access enable */
__sfr __at(0xC7) IAP_CONTR; /* 0x60 = SWBS|SWRST: soft reset into ROM ISP */

/* 12-bit SAR ADC (addresses from vendor demo STC8H.h) */
__sfr __at(0xBC) ADC_CONTR; /* bit7 POWER, bit6 START, bit5 FLAG, bits3:0 channel */
__sfr __at(0xBD) ADC_RES;
__sfr __at(0xBE) ADC_RESL;
__sfr __at(0xDE) ADCCFG;    /* bit5 RESFMT (right-justified), bits3:0 SPEED */

/* USB device controller (addresses from vendor demo STC8H.h) */
__sfr __at(0xDC) USBCLK;
__sfr __at(0xEC) USBDAT;
__sfr __at(0xF4) USBCON;
__sfr __at(0xFC) USBADR;
/* XFR registers: require P_SW2 bit7 (EAXFR) set while accessed */
#define IRC48MCR (*(__xdata volatile uint8_t *)0xFE07)
#define ADCTIM   (*(__xdata volatile uint8_t *)0xFEA8)

__sbit __at(0xAF) EA;

/* Bit-addressable pins used by the board (see board.h) */
__sbit __at(0x80) P0_0;
__sbit __at(0x81) P0_1;
__sbit __at(0x82) P0_2;
__sbit __at(0x83) P0_3;
__sbit __at(0x92) P1_2;
__sbit __at(0x93) P1_3;
__sbit __at(0x94) P1_4;
__sbit __at(0x95) P1_5;
__sbit __at(0x96) P1_6;
__sbit __at(0x97) P1_7;
__sbit __at(0xB2) P3_2;
__sbit __at(0xB5) P3_5;
__sbit __at(0xB6) P3_6;
__sbit __at(0xCC) P5_4;

#endif
