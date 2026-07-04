/*
 * usi_spi.c — serve the self-test result frames as an SPI mode-0 slave on
 * the ISP pins, using the USI in three-wire mode (DI=PB0, DO=PB1, USCK=PB2).
 * See the CFG_REPORT_USI block in config.h for the protocol and wiring.
 *
 * Polled, no interrupts: the report loop calls usi_report_poll() every
 * ~100 us.  After the 8th clock USIDR holds the received (garbage) byte
 * until the next poll reloads it, so the master must leave a short gap
 * between bytes; a torn byte fails the check-byte sum — read again.
 */
#include <avr/io.h>
#include <stdint.h>
#include "config.h"

#if CFG_REPORT_USI

#define USI_STREAM_LEN (N_FRAMES + 1)   /* 9 report bytes + check byte */

static const uint8_t *tx;       /* report frames (NULL until init) */
static uint8_t check;           /* 0x100 - sum(frames): stream sums to 0 */
static uint8_t idx;             /* stream index currently loaded in USIDR */
static uint8_t prev_cnt;        /* USI bit-counter stall detection */
static uint8_t stale;

static uint8_t
byte_at(uint8_t i)
{
    return (i < N_FRAMES) ? tx[i] : check;
}

void
usi_report_init(const uint8_t *f)
{
    uint8_t s = 0, i;

    tx = f;
    for (i = 0; i < N_FRAMES; i++)
        s += f[i];
    check = (uint8_t)(0x100 - s);

    DDRB &= (uint8_t)~(_BV(PB0) | _BV(PB2));    /* DI, USCK inputs */
#ifdef CFG_USI_CS_PIN
    CFG_USI_CS_PORT |= _BV(CFG_USI_CS_BIT);     /* pullup: float = deselected */
#else
    DDRB |= _BV(PB1);                           /* no CS: stream continuously */
#endif
    USICR = _BV(USIWM0) | _BV(USICS1);          /* 3-wire slave, ext clk, mode 0 */
    idx = 0;
    USIDR = byte_at(0);
    USISR = _BV(USIOIF);                        /* clear flag + bit counter */
}

void
usi_report_poll(void)
{
    if (!tx)
        return;
#ifdef CFG_USI_CS_PIN
    if (CFG_USI_CS_PIN & _BV(CFG_USI_CS_BIT)) { /* deselected */
        DDRB &= (uint8_t)~_BV(PB1);             /* release DO */
        idx = 0;
        USIDR = byte_at(0);
        USISR = _BV(USIOIF);                    /* resync bit counter */
        return;
    }
    DDRB |= _BV(PB1);
#endif
    if (USISR & _BV(USIOIF)) {                  /* a byte went out */
        idx = (uint8_t)((idx + 1) % USI_STREAM_LEN);
        USIDR = byte_at(idx);
        USISR = _BV(USIOIF);
        stale = 0;
        return;
    }

    /* Gap resync: the USI counts both SCK edges and a stray edge would
     * misalign the stream for good, so a bit counter stuck mid-byte for
     * ~10 ms (>=100 polls) resets everything to the sync byte.  A master
     * without CS gets a deterministic read the same way: idle the clock
     * ~20 ms, then clock a full frame. */
    {
        uint8_t cnt = USISR & 0x0F;
        if (cnt && cnt == prev_cnt) {
            if (++stale >= 100) {
                idx = 0;
                USIDR = byte_at(0);
                USISR = _BV(USIOIF);
                stale = 0;
            }
        } else {
            stale = 0;
        }
        prev_cnt = cnt;
    }
}

#endif /* CFG_REPORT_USI */
