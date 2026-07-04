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

/* ---- Result reporting -------------------------------------------------
 *
 * CFG_REPORT_HVPP: classic bench reporting — result byte on PA0-7, frame
 * strobe on PB0, read through the HVPP socket wiring (host/run_selftest.py).
 *
 * CFG_REPORT_USI: in-circuit reporting — serve the results as an SPI mode-0
 * slave on the ISP pins via the USI (DI=PB0/MOSI, DO=PB1/MISO, USCK=PB2/SCK),
 * so an SPI master already sharing the ISP bus can read them with no extra
 * wiring.  Stream = the 9 report bytes plus a check byte chosen so the 8-bit
 * sum of all 10 is zero, repeating.  MOSI content is ignored.
 *
 * The two conflict on PB0 (strobe vs. DI), so enabling USI turns HVPP
 * reporting off unless CFG_REPORT_HVPP is forced — never force both with an
 * external SPI master attached.  The USI build also leaves PA0-7 as inputs:
 * nothing is driven except DO (and the beacon if enabled), for in-circuit
 * safety.  In-circuit, remember the suite REPLACES application flash and
 * CFG_TEST_EEPROM erases the EEPROM.
 *
 * Chip select (recommended in-circuit; board-specific, so define all three
 * or none), active low, internal pullup enabled so floating = deselected.
 * DO is released while deselected, and the stream restarts at the 0xA5 sync
 * on each assertion, so a "assert CS, clock 10 bytes" read is deterministic:
 *   make FEATURES='-DCFG_REPORT_USI=1 -DCFG_USI_CS_PORT=PORTA \
 *                  -DCFG_USI_CS_PIN=PINA -DCFG_USI_CS_BIT=3'
 * Without CS, DO is driven continuously: bench only, never on a shared bus.
 *
 * Timing: reloads are polled, so keep SCK modest (<= ~50 kHz at the default
 * 1 MHz core clock) and allow a small gap between bytes; a torn byte fails
 * the check-byte test — just read again.
 */
#ifndef CFG_REPORT_USI
#define CFG_REPORT_USI 0
#endif

#ifndef CFG_REPORT_HVPP
#define CFG_REPORT_HVPP (!CFG_REPORT_USI)
#endif

#ifndef CFG_BEACON          /* 62.5 Hz clock beacon on PB4 (XTAL1 wire) */
#define CFG_BEACON CFG_REPORT_HVPP
#endif

#define N_FRAMES 9          /* report bytes; see main.c frame map */

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
