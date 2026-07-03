/*
 * 12-bit ADC + VCC sag guard (target-only, like pindbg).
 *
 * Channel map on this board — die-level, package-independent (confirmed
 * against the STC8H8K64U_Features.pdf 2.1.3 pin table, 2026-07-03):
 *   ch0/1  = P1.0/P1.1  UART2 header pads (free analog probes on USB CDC)
 *   ch2    = P5.4       RESET_DRV base node (hi-Z signature ~2.5 V)
 *   ch3-7  = P1.3 WR, P1.4 VCC_EN, P1.5 ISP-RST, P1.6 LED, P1.7 OE
 *   ch8-11 = P0.0 XTAL1, P0.1 RDY, P0.2 XA1, P0.3 PAGEL
 *   ch15   = internal 1.19 V bandgap -> our own VCC (board ties Vref+ to VCC)
 * P2 (DATA bus) and P3 have no ADC channels.
 */

#ifndef ADC_H
#define ADC_H

#ifndef __SDCC
#error "adc.h is target-only; host builds must not include it"
#endif

#include <stdint.h>

#define ADC_CH_RSTDRV  2
#define ADC_CH_BGV     15

void     adc_init(void);
uint16_t adc_read(uint8_t ch);        /* one conversion, 0..4095, ~25 us */
uint16_t adc_read_avg16(uint8_t ch);
uint16_t adc_vcc_mv(void);            /* own rail via bandgap */
uint16_t adc_ch_mv(uint8_t ch);       /* pin voltage in mV, VCC-ratiometric */

/* VCC sag guard — overcurrent proxy without a shunt: target current sags
 * the shared 5 V rail across the USB cable/polyfuse impedance.  Armed by
 * hal_vcc(1); sampled from inside hal_delay_ms / hal_rdy_wait loops (one
 * raw conversion per ~1 ms tick, so proven HVPP timings stretch <3%).
 * First sample after arming is the session baseline; two consecutive
 * samples >12% below it cut VCC_EN and latch adc_guard_fault.  Watches
 * the STC's own rail, so the socket-local light-load wobble (behind the
 * PNP) cannot false-trip it. */
void adc_guard_arm(uint8_t on);       /* called by hal_vcc() */
void adc_guard_check(void);           /* cheap early-out when disarmed */
extern __bit adc_guard_enable;        /* master switch (PINDBG 'G') */
extern __bit adc_guard_fault;         /* latched trip; cleared by 'G' */

#endif
