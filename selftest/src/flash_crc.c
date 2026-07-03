/*
 * flash_crc.c — self-read CRC of the whole flash via LPM.
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB first) over bytes
 * 0x000..FLASHEND-2; the reference value sits big-endian in the last two
 * bytes, patched in by tools/patch_crc.py at build time.
 *
 * avrdude's post-write verify already proves the array from outside; this
 * proves the LPM fetch path the running core actually uses.
 */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include "config.h"

#if CFG_TEST_FLASHCRC

uint8_t
flash_crc_test(void)
{
    uint16_t crc = 0xFFFF;
    uint16_t addr;
    uint8_t  i;

    for (addr = 0; addr < (uint16_t)FLASHEND - 1u; addr++) {
        crc ^= (uint16_t)pgm_read_byte(addr) << 8;
        for (i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? (uint16_t)(crc << 1) ^ 0x1021u
                                  : (uint16_t)(crc << 1);
    }

    {
        uint16_t ref = ((uint16_t)pgm_read_byte((uint16_t)FLASHEND - 1u) << 8)
                     | pgm_read_byte((uint16_t)FLASHEND);
        return (crc == ref) ? 0 : 1;
    }
}

#endif /* CFG_TEST_FLASHCRC */
