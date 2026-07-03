/*
 * eeprom_test.c — full-array EEPROM write/verify: 0xAA, 0x55, then 0xFF.
 *
 * DESTRUCTIVE: prior contents are lost; array is left in the erased state
 * (0xFF).  ~2.6 s at 3.4 ms/write x 256 bytes x 3 passes.  Runs through
 * to the end even after a failure so a single bad cell doesn't hide the
 * rest of the picture.
 */
#include <avr/io.h>
#include <avr/eeprom.h>
#include <stdint.h>
#include "config.h"

#if CFG_TEST_EEPROM

static uint8_t
ee_pass(uint8_t pattern)
{
    uint16_t a;
    uint8_t bad = 0;

    for (a = 0; a <= E2END; a++) {
        eeprom_write_byte((uint8_t *)a, pattern);
        if (eeprom_read_byte((uint8_t *)a) != pattern)
            bad = 1;
    }
    return bad;
}

uint8_t
eeprom_test(void)
{
    uint8_t bad = 0;

    bad |= ee_pass(0xAA);
    bad |= ee_pass(0x55);
    bad |= ee_pass(0xFF);   /* leave erased */
    return bad;
}

#endif /* CFG_TEST_EEPROM */
