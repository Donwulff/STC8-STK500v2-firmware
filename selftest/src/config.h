/*
 * config.h — compile-time feature selection for the t461a self-test image.
 *
 * Every test is a CFG_ switch so partial images can be built when flash
 * space or bench time is tight, e.g.:
 *     make FEATURES='-DCFG_TEST_EEPROM=0 -DCFG_TEST_WDT=0'
 *
 * Defaults: everything on (full suite).
 */
#ifndef CONFIG_H
#define CONFIG_H

#ifndef CFG_TEST_CPU        /* CPU register checkerboard walk (cpu_regs.S) */
#define CFG_TEST_CPU 1
#endif

#ifndef CFG_TEST_SRAM       /* March C- over ALL of SRAM at .init3 (march.S) */
#define CFG_TEST_SRAM 1
#endif

#ifndef CFG_TEST_FLASHCRC   /* CRC-16/CCITT self-read vs. reference in last 2 bytes */
#define CFG_TEST_FLASHCRC 1
#endif

#ifndef CFG_TEST_EEPROM     /* AA/55/FF full-array write+verify — DESTRUCTIVE,
                             * leaves EEPROM erased (0xFF).  ~2.6 s. */
#define CFG_TEST_EEPROM 1
#endif

#ifndef CFG_TEST_ADC        /* VCC via internal 1.1 V bandgap (MUX5:0=011110) */
#define CFG_TEST_ADC 1
#endif

#ifndef CFG_TEST_WDT        /* arm 250 ms watchdog, verify it actually resets us */
#define CFG_TEST_WDT 1
#endif

#ifndef CFG_DRIVE_RESET_HIGH   /* DRIVE-FIGHT EXPERIMENT, default OFF:
                                * drive PB7 (ex-RESET, needs RSTDISBL) high
                                * in the report loop so an external pull-low
                                * fights the pin.  ~35 mA into a hard short —
                                * visible to the STC sag guard.  Prefer the
                                * 100R ISP-header path (Option B wiring) and
                                * keep sessions short: this creates a
                                * pin-stressing drive fight ON PURPOSE. */
#define CFG_DRIVE_RESET_HIGH 0
#endif

/* ADC pass window, millivolts.  Board rail measured ~4.1-4.2 V under load. */
#ifndef CFG_ADC_MV_MIN
#define CFG_ADC_MV_MIN 3300
#endif
#ifndef CFG_ADC_MV_MAX
#define CFG_ADC_MV_MAX 5000
#endif

/* avr-libc always defines RAMEND; RAMSTART appeared later — be safe. */
#ifndef RAMSTART
#define RAMSTART 0x60
#endif

#endif /* CONFIG_H */
