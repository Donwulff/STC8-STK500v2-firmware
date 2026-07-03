/*
 * ISP (serial programming) over the HVPP socket — no rewiring needed.
 *
 * The straight name-to-name HVPP wiring for 20-pin x61 parts happens to BE
 * a complete ISP rig: header WR -> ZIF 1 = PB0 = MOSI, XA0 -> ZIF 2 = PB1 =
 * MISO, XA1 -> ZIF 3 = PB2 = SCK.  RESET must merely be held low, which is
 * the HVPP reset driver's fail-safe default (GND), and we control socket
 * VCC — so entry retries use a REAL power cycle instead of the RESET pulse
 * the protocol suggests (our reset node can only do 0 V or 12 V; for the
 * same reason a socketed chip can never be RELEASED to run: lift the RESET
 * jumper wire off the ZIF to run test firmware).
 *
 * Doubles as an independent wiring/pin-map verifier for P1.6/P3.5/P0.2 and
 * as the programmer for AVR-side test firmware (@WIREDOC).  Note: a part with
 * DWEN programmed has its ISP disabled (use HVPP for those); this module is
 * for ISP-capable chips.  Command formats per AVR068; avrdude -c stk500v2.
 *
 * Portable: SDCC (target) and gcc (host mock, byte-level target sim).
 */

#include "isp.h"
#include "stk500v2.h"
#include "stk_proto.h"

/* body: [1]timeout [2]stabDelay [3]cmdexeDelay [4]synchLoops [5]byteDelay
 *       [6]pollValue [7]pollIndex [8..11] = Programming Enable command */
int16_t
isp_enter_progmode(void)
{
    uint8_t stab   = stk_body[2];
    uint8_t cmdexe = stk_body[3];
    uint8_t loops  = stk_body[4];
    uint8_t bdelay = stk_body[5];
    uint8_t pval   = stk_body[6];
    uint8_t pix    = stk_body[7];
    uint8_t attempt, i, rx, ok = 0;

    if (!loops)
        loops = 1;
    if (loops > 8)
        loops = 8;    /* power-cycle retries are slow; 32 stock loops would
                         overrun avrdude's 5 s response timeout */
    if (pix > 4)
        pix = 4;

    hal_hv_reset(0);               /* RESET at GND = held active (default) */
    hal_isp_connect();             /* SCK low BEFORE power: entry condition */
    hal_vcc(0);
    hal_delay_ms(50);

    /* The socket rail wobbles at light load (bench-observed dips at
     * varying 10-100 ms offsets after switch-on), so one attempt per
     * power cycle loses to timing luck.  Instead: sweep the power-on
     * phase across a few cycles, and within each cycle fire MANY quick
     * attempts using the datasheet recovery (positive SCK pulse, retry
     * Programming Enable) to ride through any stable window. */
    (void)stab;
    for (attempt = 0; attempt < 6 && !ok; ++attempt) {
        uint8_t inner;
        hal_vcc(1);
        hal_delay_ms((uint8_t)(20 + attempt * 30));
        for (inner = 0; inner < 32 && !ok; ++inner) {
            if (!pix)
                ok = 1;            /* pollIndex 0 = no polling */
            for (i = 0; i < 4; ++i) {
                rx = hal_isp_xfer(stk_body[8 + i]);
                if (bdelay)
                    hal_delay_ms(bdelay);
                if (pix && i == (uint8_t)(pix - 1) && rx == pval)
                    ok = 1;
            }
            if (!ok) {
                hal_isp_sck_pulse();
                hal_delay_ms(2);
            }
        }
        if (!ok) {
            hal_vcc(0);
            hal_delay_ms(30);
        }
    }
    (void)loops;

    if (ok) {
        hal_delay_ms(cmdexe);
        stk_body[1] = STATUS_CMD_OK;
    } else {
        hal_isp_disconnect();
        hal_vcc(0);
        stk_body[1] = STATUS_CMD_TOUT;
    }
    return 2;
}

/* body: [1]preDelay [2]postDelay.  Parks the socket powered off with RESET
 * at GND — we cannot release RESET to let the chip run (0 V / 12 V node). */
int16_t
isp_leave_progmode(void)
{
    hal_delay_ms(stk_body[1]);
    hal_isp_disconnect();
    hal_vcc(0);
    hal_delay_ms(stk_body[2]);
    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

static void
xfer4(const uint8_t *cmd, uint8_t *rx)
{
    uint8_t i, r;
    for (i = 0; i < 4; ++i) {
        r = hal_isp_xfer(cmd[i]);
        if (rx)
            rx[i] = r;
    }
}

/* Poll RDY/BSY (0xF0): LSB of the 4th byte is 1 while busy. */
static uint8_t
rdy_poll(uint8_t timeout_ms)
{
    static const uint8_t poll_cmd[4] = { 0xF0, 0x00, 0x00, 0x00 };
    uint8_t rx[4];
    uint8_t n = timeout_ms ? timeout_ms : 100;

    while (n--) {
        xfer4(poll_cmd, rx);
        if (!(rx[3] & 0x01))
            return 1;
        hal_delay_ms(1);
    }
    return 0;
}

/* body: [1]eraseDelay [2]pollMethod [3..6] = Chip Erase command */
int16_t
isp_chip_erase(void)
{
    xfer4(&stk_body[3], 0);
    if (stk_body[2]) {
        if (!rdy_poll(200)) {
            stk_body[1] = STATUS_RDY_BSY_TOUT;
            return 2;
        }
    }
    hal_delay_ms(stk_body[1]);
    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

/* body: [1]numTx [2]numRx [3]rxStartAddr [4..] txData
 * answer: [cmd, OK, rx bytes (numRx), OK] */
int16_t
isp_spi_multi(void)
{
    uint8_t ntx = stk_body[1];
    uint8_t nrx = stk_body[2];
    uint8_t rs  = stk_body[3];
    uint8_t total = (uint8_t)(rs + nrx);
    uint8_t i, rx, k = 0;

    if ((uint16_t)rs + nrx > 255) {    /* nrx itself always fits the body */
        stk_body[1] = STATUS_CMD_FAILED;
        return 2;
    }
    if (ntx > total)
        total = ntx;
    for (i = 0; i < total; ++i) {
        rx = hal_isp_xfer(i < ntx ? stk_body[4 + i] : 0x00);
        if (i >= rs && k < nrx)
            stk_body[2 + k++] = rx;    /* trails the tx reads: no overlap */
    }
    stk_body[1] = STATUS_CMD_OK;
    stk_body[2 + nrx] = STATUS_CMD_OK;
    return (int16_t)(3 + nrx);
}

/* READ_FUSE/LOCK/SIGNATURE/OSCCAL_ISP share one format:
 * body: [1]retAddr [2..5] = command; answer: [cmd, OK, value, OK].
 * RetAddr is the 1-BASED SPI byte position holding the data (avrdude
 * sends 4 = last byte). */
int16_t
isp_read_byte_cmd(void)
{
    uint8_t ra = stk_body[1];
    uint8_t rx[4];

    ra = (ra >= 1 && ra <= 4) ? (uint8_t)(ra - 1) : 3;
    xfer4(&stk_body[2], rx);
    stk_body[1] = STATUS_CMD_OK;
    stk_body[2] = rx[ra];
    stk_body[3] = STATUS_CMD_OK;
    return 4;
}

/* PROGRAM_FUSE/LOCK_ISP: body [1..4] = command; answer [cmd, OK, OK].
 * No delay field in the frame; worst-case fuse write is ~4.5 ms. */
int16_t
isp_program_byte_cmd(void)
{
    xfer4(&stk_body[1], 0);
    hal_delay_ms(10);
    stk_body[1] = STATUS_CMD_OK;
    stk_body[2] = STATUS_CMD_OK;
    return 3;
}

/* READ_FLASH/EEPROM_ISP: body [1]lenH [2]lenL [3]cmd1 (read low).
 * Flash is word-addressed via stk_address, low byte then cmd1|0x08 for
 * high; EEPROM is byte-addressed. */
static int16_t
read_mem(uint8_t flash)
{
    uint16_t n = ((uint16_t)stk_body[1] << 8) | stk_body[2];
    uint8_t cmd1 = stk_body[3];
    uint16_t i = 2;
    uint8_t hi = 0;

    if (n > STK_MAX_BODY - 3) {
        stk_body[1] = STATUS_CMD_FAILED;
        return 2;
    }
    while (n--) {
        hal_isp_xfer((flash && hi) ? (uint8_t)(cmd1 | 0x08) : cmd1);
        hal_isp_xfer((uint8_t)(stk_address >> 8));
        hal_isp_xfer((uint8_t)stk_address);
        stk_body[i++] = hal_isp_xfer(0x00);
        if (!flash || hi)
            ++stk_address;
        if (flash)
            hi = !hi;
    }
    stk_body[i++] = STATUS_CMD_OK;
    stk_body[1]   = STATUS_CMD_OK;
    return (int16_t)i;
}

int16_t isp_read_flash(void)  { return read_mem(1); }
int16_t isp_read_eeprom(void) { return read_mem(0); }

/* PROGRAM_FLASH/EEPROM_ISP: body [1]lenH [2]lenL [3]mode [4]delay
 * [5]cmd1(load/write) [6]cmd2(write page) [7]cmd3 [8]poll1 [9]poll2
 * [10..]data.  Value-polling is skipped in favor of the caller-supplied
 * max write delay — slower but unconditionally safe. */
static int16_t
program_mem(uint8_t flash)
{
    uint16_t n = ((uint16_t)stk_body[1] << 8) | stk_body[2];
    uint8_t mode  = stk_body[3];
    uint8_t delay = stk_body[4];
    uint8_t cmd1  = stk_body[5];
    uint8_t cmd2  = stk_body[6];
    uint16_t i = 10;
    uint8_t hi = 0;
    uint32_t page_addr = stk_address;

    if (n > STK_MAX_BODY - 10) {
        stk_body[1] = STATUS_CMD_FAILED;
        return 2;
    }
    while (n--) {
        hal_isp_xfer((flash && hi) ? (uint8_t)(cmd1 | 0x08) : cmd1);
        hal_isp_xfer((uint8_t)(stk_address >> 8));
        hal_isp_xfer((uint8_t)stk_address);
        hal_isp_xfer(stk_body[i++]);
        if (!(mode & 0x01))
            hal_delay_ms(delay ? delay : 10);   /* word mode: wait per byte */
        if (!flash || hi)
            ++stk_address;
        if (flash)
            hi = !hi;
    }
    if ((mode & 0x01) && (mode & 0x80)) {       /* page mode: commit page */
        hal_isp_xfer(cmd2);
        hal_isp_xfer((uint8_t)(page_addr >> 8));
        hal_isp_xfer((uint8_t)page_addr);
        hal_isp_xfer(0x00);
        hal_delay_ms(delay ? delay : 10);
    }
    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

int16_t isp_program_flash(void)  { return program_mem(1); }
int16_t isp_program_eeprom(void) { return program_mem(0); }
