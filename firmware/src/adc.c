/*
 * 12-bit SAR ADC driver + VCC sag guard for the STC8H8K64U.
 *
 * ADCTIM 0x3F (CSHOLD=1, SMPDUTY=31) = longest sampling window: the nodes
 * we probe are high impedance (hi-Z pins behind clamp networks), and the
 * S/H cap must charge through them.  ADCCFG SPEED=5: Fadc = 24M/2/6 =
 * 2 MHz, one conversion ~47 ADC clocks ~ 25 us.
 *
 * Absolute accuracy rides on the internal bandgap (1.19 V nominal, no
 * per-chip cal applied) — good to a few %, plenty for sag detection and
 * pin-to-pin comparisons, which are the FA use cases.
 */

#include "board.h"
#include "hal.h"
#include "adc.h"

__bit adc_guard_enable = 1;
__bit adc_guard_fault;
static __bit armed;
static __xdata uint16_t base_raw;  /* bandgap raw at session start; 0 = pending */
static __xdata uint8_t trip_count;

void
adc_init(void)
{
    P_SW2 |= 0x80;
    ADCTIM = 0x3F;
    P_SW2 &= (uint8_t)~0x80;
    ADCCFG = 0x25;             /* RESFMT right-justified | SPEED=5 */
    ADC_CONTR = 0x80;          /* power the SAR block */
    hal_delay_ms(1);           /* datasheet: settle before first start */
}

uint16_t
adc_read(uint8_t ch)
{
    ADC_CONTR = (uint8_t)(0xC0 | ch);          /* POWER | START | channel */
    __asm__("nop"); __asm__("nop");
    __asm__("nop"); __asm__("nop");            /* vendor demo: gap before flag poll */
    while (!(ADC_CONTR & 0x20))
        ;
    ADC_CONTR &= (uint8_t)~0x20;               /* clear FLAG, keep POWER */
    return (uint16_t)(((uint16_t)ADC_RES << 8) | ADC_RESL);
}

uint16_t
adc_read_avg16(uint8_t ch)
{
    uint16_t acc = 0;                          /* 16 * 4095 fits uint16_t */
    uint8_t i;

    (void)adc_read(ch);                        /* throwaway after mux switch */
    for (i = 0; i < 16; ++i)
        acc += adc_read(ch);
    return acc >> 4;
}

uint16_t
adc_vcc_mv(void)
{
    uint16_t raw = adc_read_avg16(ADC_CH_BGV);

    if (raw == 0)
        return 0;
    return (uint16_t)(4874240UL / raw);        /* 1190 mV * 4096 / raw */
}

uint16_t
adc_ch_mv(uint8_t ch)
{
    return (uint16_t)(((uint32_t)adc_read_avg16(ch) * adc_vcc_mv()) >> 12);
}

void
adc_guard_arm(uint8_t on)
{
    armed = on ? 1 : 0;
    base_raw = 0;
    trip_count = 0;
}

void
adc_guard_check(void)
{
    uint16_t raw;

    if (!armed || !adc_guard_enable)
        return;
    raw = adc_read(ADC_CH_BGV);
    if (base_raw == 0) {                       /* first tick after VCC on */
        base_raw = raw;
        return;
    }
    /* bandgap raw RISES as VCC sags; base/8 = ~12% rail sag (~0.5 V) */
    if (raw > base_raw + (base_raw >> 3)) {
        if (++trip_count >= 2) {
            hal_vcc(0);                        /* also disarms via arm(0) */
            adc_guard_fault = 1;
        }
    } else {
        trip_count = 0;
    }
}
