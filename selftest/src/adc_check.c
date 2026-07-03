/*
 * adc_check.c — VCC measurement via the internal 1.1 V bandgap.
 *
 * Same trick as the STC's shuntless guard, from the other side of the
 * socket: convert the bandgap against a VCC reference, then
 * VCC = 1100 mV * 1024 / raw.
 *
 * x61 channel map (datasheet 2588F Table 15-4): the single-ended bandgap
 * input is MUX5:0 = 0b011110 -> ADMUX = 0x1E with ADCSRB.MUX5 = 0.
 * (0b011111 is AGND; the 0b111110 row further down is a differential
 * entry — easy to grab the wrong one.)  Nominal V_BG is 1.1 V (2588F
 * changelog: "changed INTERNAL 1.18V REFERENCE to 1.1V").
 */
#include <avr/io.h>
#include <util/delay.h>
#include <stdint.h>
#include "config.h"

#if CFG_TEST_ADC

uint16_t
adc_vcc_mv(void)
{
    uint16_t acc = 0;
    uint8_t  i;

    ADMUX  = 0x1E;                          /* VCC ref, right-adj, bandgap in */
    ADCSRB = 0;                             /* MUX5 = 0 */
    ADCSRA = _BV(ADEN) | _BV(ADPS1) | _BV(ADPS0);   /* /8 -> 125 kHz @ 1 MHz */
    _delay_ms(2);                           /* bandgap start-up */

    for (i = 0; i < 12; i++) {              /* discard 4, average 8 */
        ADCSRA |= _BV(ADSC);
        while (ADCSRA & _BV(ADSC))
            ;
        if (i >= 4)
            acc += ADC;
    }
    ADCSRA = 0;

    acc >>= 3;
    if (acc < 64)                           /* < ~0.27 VCC=17V?! wrong channel */
        return 0;
    return (uint16_t)(1126400UL / acc);     /* 1100 mV * 1024 */
}

#endif /* CFG_TEST_ADC */
