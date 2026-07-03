/*
 * HVPP control-stack playback engine.
 * Port of ScratchMonkey SMoHVPP.cpp (c) 2013-2016 Matthias Neeracher, BSD,
 * with stack auto-remap added; compiles for both SDCC (target) and gcc (host).
 */

#include "hvpp.h"
#include "stk500v2.h"
#include "stk_proto.h"

/* Control stack indices (AVRminiProg convention, as used by avrdude) */
enum {
    kLoadAddr    =  0,
    kLoadData    =  4,
    kLoadCommand =  8,
    kDone        = 12,
    kCommitData  = 16,
    kEnableRead  = 20,
    kPageLoad    = 24,
    kInit        = 28,

    kLowByte     = 0,
    kHighByte    = 1,
    kExtByte     = 2,
    kExt2Byte    = 3
};

XDATA uint8_t hvpp_stack[32];
XDATA uint8_t hvpp_stack_raw[32];
uint8_t hvpp_remap_mode = HVPP_REMAP_AUTO;
uint8_t hvpp_family     = HVPP_FAMILY_STD;

uint8_t
hvpp_classify(const uint8_t *cs)
{
    /*
     * x61 fingerprint (t261/t461/t861 pp_controlstack):
     *  - enableRead entries keep bit2 high (x61 WR inactive) and drop bit7
     *    (x61 OE active) — the standard stack does the exact opposite;
     *  - commit drops bit2 while done keeps it (WR pulsed on bit2, not bit3).
     */
    if ((cs[kEnableRead] & 0x84) == 0x04 &&
        (cs[kDone] & 0x04) && !(cs[kCommitData] & 0x04))
        return HVPP_FAMILY_X61;
    return HVPP_FAMILY_STD;
}

static uint8_t
remap_x61(uint8_t v)
{
    uint8_t r = 0;
    if (v & 0x04) r |= 0x08;    /* x61 WR        -> WR   (CTRL3) */
    if (v & 0x08) r |= 0x20;    /* x61 XA0       -> XA0  (CTRL5) */
    if (v & 0x10) r |= 0x40;    /* x61 XA1/BS2   -> XA1  (CTRL6) */
    if (v & 0x20) r |= 0x10;    /* x61 PAGEL/BS1 -> BS1  (CTRL4) */
    if (v & 0x80) r |= 0x04;    /* x61 OE        -> OE   (CTRL2) */
    return r;                   /* bit6 parked, bits 0/1 unused: dropped */
}

void
hvpp_apply_remap(void)
{
    uint8_t i;
    if (hvpp_remap_mode == HVPP_REMAP_RAW)
        hvpp_family = HVPP_FAMILY_STD;
    else if (hvpp_remap_mode == HVPP_REMAP_X61)
        hvpp_family = HVPP_FAMILY_X61;
    else
        hvpp_family = hvpp_classify(hvpp_stack_raw);

    for (i = 0; i < 32; ++i)
        hvpp_stack[i] = (hvpp_family == HVPP_FAMILY_X61)
                      ? remap_x61(hvpp_stack_raw[i])
                      : hvpp_stack_raw[i];
}

void
hvpp_set_stack(const uint8_t *raw)
{
    uint8_t i;
    for (i = 0; i < 32; ++i)
        hvpp_stack_raw[i] = raw[i];
    hvpp_apply_remap();
}

/* ---- playback primitives ---- */

static void
set_ctrl(uint8_t ix)
{
    hal_ctrl_set(hvpp_stack[ix]);
    hal_delay_us(1);
}

static void
write_data(uint8_t ix, uint8_t v)
{
    set_ctrl(ix);
    hal_data_set(v);
    hal_xtal_pulse();
}

static void
load_command(uint8_t cmd)
{
    hal_data_mode(1);
    write_data(kLoadCommand, cmd);
}

static void
load_address(uint8_t sel, uint8_t v)
{
    write_data(kLoadAddr + sel, v);
}

static void
load_data(uint8_t sel, uint8_t v)
{
    write_data(kLoadData + sel, v);
}

static void
commit_data(uint8_t sel, uint8_t pulse_ms)
{
    set_ctrl(kDone + sel);
    set_ctrl(kCommitData + sel);
    if (pulse_ms)
        hal_delay_ms(pulse_ms);
    set_ctrl(kDone + sel);
}

static uint8_t
read_data(uint8_t sel)
{
    set_ctrl(kLoadData + sel);
    set_ctrl(kEnableRead + sel);
    return hal_data_get();
}

/* ---- command handlers ---- */

int16_t
hvpp_enter_progmode(void)
{
    /* body: [1]stabDelay [2]cmdexeDelay [3]latchCycles [4]toggleVtg
             [5]powOffDelay [6]resetDelay1(ms) [7]resetDelay2(us) */
    uint8_t stab_ms      = stk_body[1];
    uint8_t latch_cycles = stk_body[3];
    uint8_t pow_off      = stk_body[5];
    uint8_t rst_ms       = stk_body[6];
    uint8_t rst_us       = stk_body[7];
    uint8_t i;

    hal_vcc(0);
    hal_hv_reset(0);                       /* RESET held at GND */
    hal_data_mode(1);
    hal_ctrl_engage();                     /* push-pull drive for the session */
    hal_ctrl_set(hvpp_stack[kInit]);
    hal_delay_ms(pow_off);
    hal_vcc(1);
    hal_delay_us(50);
    for (i = 0; i < latch_cycles; ++i)
        hal_xtal_pulse();
    hal_delay_ms(stab_ms);                 /* socket VCC settle (avrdude
                                              hventerstabdelay, t461a: 100 ms);
                                              our VCC switch + rail caps are
                                              slower than a bare MCU pin */
    hal_hv_reset(1);                       /* 12 V on RESET */
    hal_delay_ms(rst_ms);
    hal_delay_us(rst_us);
    set_ctrl(kDone);

    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

int16_t
hvpp_leave_progmode(void)
{
    uint8_t rst_ms = stk_body[2];

    hal_hv_reset(0);                       /* back to GND, never 12 V at idle */
    hal_vcc(0);
    hal_ctrl_release();
    hal_data_mode(0);
    hal_delay_ms(rst_ms);

    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

int16_t
hvpp_chip_erase(void)
{
    uint8_t pulse_width  = stk_body[1];
    uint8_t poll_timeout = stk_body[2];

    load_command(0x80);
    commit_data(kLowByte, pulse_width);

    if (poll_timeout && !hal_rdy_wait(poll_timeout)) {
        stk_body[1] = STATUS_RDY_BSY_TOUT;
        return 2;
    }
    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

static int16_t
read_sig_cal(uint8_t addr, uint8_t sel)
{
    load_command(0x08);
    load_address(kLowByte, addr);
    hal_data_mode(0);
    stk_body[2] = read_data(sel);
    set_ctrl(kDone);
    hal_data_mode(1);

    stk_body[1] = STATUS_CMD_OK;
    stk_body[3] = STATUS_CMD_OK;
    return 4;
}

int16_t
hvpp_read_signature(void)
{
    return read_sig_cal(stk_body[1], kLowByte);
}

int16_t
hvpp_read_osccal(void)
{
    return read_sig_cal(0x00, kHighByte);
}

static int16_t
read_fuse_lock(uint8_t sel)
{
    load_command(0x04);
    hal_data_mode(0);
    stk_body[2] = read_data(sel);
    set_ctrl(kDone);
    hal_data_mode(1);

    stk_body[1] = STATUS_CMD_OK;
    stk_body[3] = STATUS_CMD_OK;
    return 4;
}

int16_t
hvpp_read_fuse(void)
{
    uint8_t addr = stk_body[1];
    /* enableRead byteSel layout: 0=lfuse 1=lock 2=efuse 3=hfuse */
    return read_fuse_lock(addr == 1 ? kExt2Byte : addr);
}

int16_t
hvpp_read_lock(void)
{
    return read_fuse_lock(kHighByte);
}

static int16_t
prog_fuse_lock(uint8_t cmd, uint8_t sel)
{
    uint8_t value        = stk_body[2];
    uint8_t pulse_width  = stk_body[3];
    uint8_t poll_timeout = stk_body[4];

    load_command(cmd);
    load_data(kLowByte, value);
    commit_data(sel, pulse_width);

    if (!hal_rdy_wait(poll_timeout)) {
        stk_body[1] = STATUS_RDY_BSY_TOUT;
        return 2;
    }
    stk_body[1] = STATUS_CMD_OK;
    return 2;
}

int16_t
hvpp_program_fuse(void)
{
    return prog_fuse_lock(0x40, stk_body[1]);
}

int16_t
hvpp_program_lock(void)
{
    return prog_fuse_lock(0x20, kLowByte);
}

int16_t
hvpp_read_flash(void)
{
    uint16_t n = ((uint16_t)stk_body[1] << 8) | stk_body[2];
    uint16_t i = 2;
    uint16_t page, prev_page = 0xFFFF;

    if (n > STK_MAX_BODY - 3) {
        stk_body[1] = STATUS_CMD_FAILED;
        return 2;
    }

    load_command(0x02);
    while (n) {
        page = (uint16_t)(stk_address >> 8);
        if (page != prev_page) {
            if (stk_address & 0x80000000UL)
                load_address(kExtByte, (uint8_t)(stk_address >> 16));
            load_address(kHighByte, (uint8_t)(stk_address >> 8));
            prev_page = page;
        }
        load_address(kLowByte, (uint8_t)stk_address);
        hal_data_mode(0);
        stk_body[i++] = read_data(kLowByte);
        stk_body[i++] = read_data(kHighByte);
        set_ctrl(kDone);
        hal_data_mode(1);
        ++stk_address;
        n = (n >= 2) ? n - 2 : 0;
    }
    stk_body[i++] = STATUS_CMD_OK;
    stk_body[1]   = STATUS_CMD_OK;
    return (int16_t)i;
}

int16_t
hvpp_read_eeprom(void)
{
    uint16_t n = ((uint16_t)stk_body[1] << 8) | stk_body[2];
    uint16_t i = 2;
    uint16_t page, prev_page = 0xFFFF;

    if (n > STK_MAX_BODY - 3) {
        stk_body[1] = STATUS_CMD_FAILED;
        return 2;
    }

    load_command(0x03);
    while (n--) {
        page = (uint16_t)(stk_address >> 8);
        if (page != prev_page) {
            load_address(kHighByte, (uint8_t)(stk_address >> 8));
            prev_page = page;
        }
        load_address(kLowByte, (uint8_t)stk_address);
        hal_data_mode(0);
        stk_body[i++] = read_data(kLowByte);
        set_ctrl(kDone);
        hal_data_mode(1);
        ++stk_address;
    }
    stk_body[i++] = STATUS_CMD_OK;
    stk_body[1]   = STATUS_CMD_OK;
    return (int16_t)i;
}
