/*
 * main.c — ATtiny461A on-target self-test, reporting through the HVPP
 * socket wiring of the STC8 programmer board (straight x61 wiring, chip
 * pins per datasheet 2588F Table 18-12 — BENCH-VERIFIED 2026-07-03):
 *
 *   PA0-7 = result byte bus  -> DATA header -> STC P2   (pindbg 'p', P2=xx)
 *   PB0   = frame strobe     -> WR header   -> STC P1.3 (pindbg 'p', P1 bit3)
 *           (PB0 is the x61 HVPP "WR" signal pin; RDY/BSY is PB6)
 *   PB4   = clock beacon     -> XTAL1 wire  -> STC P0.0 = ADC ch8
 *                                              (pindbg 'h' then 'W' capture)
 *   PB7   = RESET, untouched.  Other PB pins left as inputs (STC quasi-high).
 *
 * Frame stream, repeated forever; each frame = PA set, strobe high ~300 ms,
 * strobe low ~300 ms.  Position after the 0xA5 sync identifies the field:
 *
 *   0  0xA5 sync
 *   1  CPU regs   0x1A pass / 0x1F fail / 0x10 off
 *   2  SRAM march 0x2A pass / 0x2F fail / 0x20 off-or-skipped
 *   3  flash CRC  0x3A pass / 0x3F fail / 0x30 off
 *   4  EEPROM     0x4A pass / 0x4F fail / 0x40 off
 *   5  ADC/VCC    0x5A in-window / 0x5F out / 0x50 off
 *   6  WDT        0x6A fired-as-armed / 0x60 pending-or-off / 0x6E unexpected
 *   7  VCC estimate, mV/32 (0 if ADC off)
 *   8  MCUSR captured at the tested boot
 *
 * The beacon toggles every 8 ms (62.5 Hz at a true 1 MHz) through all
 * phases, so 'W' can measure the calibrated-RC clock at any time.
 *
 * WDT test: first boot runs tests 1-5, stashes the frames in .noinit
 * (which the .init3 march skips on WDRF), arms a 250 ms watchdog and
 * spins.  The reset lands back here with WDRF set and the stash valid ->
 * frame 6 = 0x6A and the report loop runs on the preserved results.
 */
#include <avr/io.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <stdint.h>
#include "config.h"

uint8_t  cpu_regs_test(void);
uint8_t  flash_crc_test(void);
uint8_t  eeprom_test(void);
uint16_t adc_vcc_mv(void);
#if CFG_REPORT_USI
void     usi_report_init(const uint8_t *f);
void     usi_report_poll(void);
#endif

struct persist {
    uint8_t magic_a;            /* 0xC3 */
    uint8_t frames[N_FRAMES];
    uint8_t sum;                /* 8-bit sum of frames[] */
    uint8_t magic_b;            /* 0x3C */
};
static struct persist stash __attribute__((section(".noinit")));

static uint8_t frames[N_FRAMES];

static uint8_t
frames_sum(const uint8_t *f)
{
    uint8_t s = 0, i;
    for (i = 0; i < N_FRAMES; i++)
        s += f[i];
    return s;
}

/* delay in 8 ms units; keeps the beacon alive and the USI slave served */
static void
beacon_delay(uint8_t n8)
{
    while (n8--) {
        uint8_t k;
        for (k = 0; k < 80; k++) {
            _delay_us(100);
#if CFG_REPORT_USI
            usi_report_poll();
#endif
        }
#if CFG_BEACON
        PINB = _BV(PB4);        /* hardware toggle */
#endif
    }
}

static void
report_forever(const uint8_t *f)
{
#if CFG_REPORT_USI
    usi_report_init(f);
#endif
    for (;;) {
#if CFG_REPORT_HVPP
        uint8_t i;
        for (i = 0; i < N_FRAMES; i++) {
            PORTA = f[i];
            PORTB |= _BV(PB0);
            beacon_delay(38);   /* ~304 ms high */
            PORTB &= (uint8_t)~_BV(PB0);
            beacon_delay(38);   /* ~304 ms low */
        }
#else
        beacon_delay(38);
#endif
    }
}

int
main(void)
{
    uint8_t m = MCUSR;
    MCUSR = 0;
    wdt_disable();              /* WDE is forced on after a WDT reset */

    DDRB = 0;
#if CFG_REPORT_HVPP
    DDRA  = 0xFF;               /* result bus + strobe: bench wiring only */
    PORTA = 0x00;
    DDRB |= _BV(PB0);
#endif
#if CFG_BEACON
    DDRB |= _BV(PB4);
#endif
#if CFG_DRIVE_RESET_HIGH
    /* deliberate reset-net drive fight — see config.h warning */
    PORTB |= _BV(PB7);
    DDRB  |= _BV(PB7);
#endif

    if ((m & _BV(WDRF)) &&
        stash.magic_a == 0xC3 && stash.magic_b == 0x3C &&
        stash.sum == frames_sum(stash.frames)) {
        stash.frames[6] = 0x6A;             /* watchdog fired as armed */
        report_forever(stash.frames);
    }

    frames[0] = 0xA5;

#if CFG_TEST_CPU
    frames[1] = cpu_regs_test() ? 0x1F : 0x1A;
#else
    frames[1] = 0x10;
#endif

#if CFG_TEST_SRAM
    {   /* .init3 march posted its verdict in GPIOR0 (0 = skipped) */
        uint8_t g = GPIOR0;
        frames[2] = (g == 0x2A || g == 0x2F) ? g : 0x20;
    }
#else
    frames[2] = 0x20;
#endif

#if CFG_TEST_FLASHCRC
    frames[3] = flash_crc_test() ? 0x3F : 0x3A;
#else
    frames[3] = 0x30;
#endif

#if CFG_TEST_EEPROM
    frames[4] = eeprom_test() ? 0x4F : 0x4A;
#else
    frames[4] = 0x40;
#endif

#if CFG_TEST_ADC
    {
        uint16_t mv = adc_vcc_mv();
        frames[5] = (mv >= CFG_ADC_MV_MIN && mv <= CFG_ADC_MV_MAX) ? 0x5A : 0x5F;
        frames[7] = (uint8_t)(mv / 32u);
    }
#else
    frames[5] = 0x50;
    frames[7] = 0;
#endif

    frames[6] = (m & _BV(WDRF)) ? 0x6E : 0x60;  /* 0x6E: WDT we never armed */
    frames[8] = m;

#if CFG_TEST_WDT
    if (!(m & _BV(WDRF))) {
        uint8_t i;
        stash.magic_a = 0xC3;
        for (i = 0; i < N_FRAMES; i++)
            stash.frames[i] = frames[i];
        stash.sum     = frames_sum(stash.frames);
        stash.magic_b = 0x3C;
        wdt_enable(WDTO_250MS);
        for (;;)
            beacon_delay(1);    /* beacon until the watchdog bites */
    }
#endif

    report_forever(frames);
}
