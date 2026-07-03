/*
 * Unit tests: control-stack fingerprint classifier + x61 remap semantics,
 * and an STK500v2 framing round-trip.
 *
 * The three 32-byte stacks below are the pp_controlstack values every
 * STK500v2 host (avrdude, AVR Studio) transmits for these parts — they
 * originate in Atmel's part-description files and arrive on our wire at
 * runtime; an interoperable implementation necessarily contains these
 * exact bytes.  Transcribed via avrdude.conf; functional protocol data,
 * not creative content (see THIRD_PARTY_NOTICES.md).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "hvpp.h"
#include "stk500v2.h"
#include "stk_proto.h"

size_t mock_take_tx(uint8_t *dst, size_t max);
extern int sim_warnings;

/* ATtiny261/461/861 (crossed: CTRL2=WR 3=XA0 4=XA1/BS2 5=PAGEL/BS1 7=OE) */
static const uint8_t stack_t261[32] = {
    0xc4, 0xe4, 0xc4, 0xe4, 0xcc, 0xec, 0xcc, 0xec,
    0xd4, 0xf4, 0xd4, 0xf4, 0xdc, 0xfc, 0xdc, 0xfc,
    0xc8, 0xe8, 0xd8, 0xf8, 0x4c, 0x6c, 0x5c, 0x7c,
    0xec, 0xbc, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00
};

/* ATtiny2313 (standard CTRL bit positions) */
static const uint8_t stack_t2313[32] = {
    0x0e, 0x1e, 0x0e, 0x1e, 0x2e, 0x3e, 0x2e, 0x3e,
    0x4e, 0x5e, 0x4e, 0x5e, 0x6e, 0x7e, 0x6e, 0x7e,
    0x26, 0x36, 0x66, 0x76, 0x2a, 0x3a, 0x6a, 0x7a,
    0x2e, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
};

/* ATmega8 (standard) */
static const uint8_t stack_m8[32] = {
    0x0e, 0x1e, 0x0f, 0x1f, 0x2e, 0x3e, 0x2f, 0x3f,
    0x4e, 0x5e, 0x4f, 0x5f, 0x6e, 0x7e, 0x6f, 0x7f,
    0x66, 0x76, 0x67, 0x77, 0x6a, 0x7a, 0x6b, 0x7b,
    0xbe, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
};

static int failures;

#define CHECK(cond, ...) do {                                   \
        if (cond) {                                             \
            printf("PASS: " __VA_ARGS__); putchar('\n');        \
        } else {                                                \
            printf("FAIL: " __VA_ARGS__); putchar('\n');        \
            ++failures;                                         \
        }                                                       \
    } while (0)

#define BIT(v, n) (((v) >> (n)) & 1)

static void
test_classifier(void)
{
    CHECK(hvpp_classify(stack_t261) == HVPP_FAMILY_X61,
          "classifier: t261 stack -> x61 family");
    CHECK(hvpp_classify(stack_t2313) == HVPP_FAMILY_STD,
          "classifier: t2313 stack -> standard");
    CHECK(hvpp_classify(stack_m8) == HVPP_FAMILY_STD,
          "classifier: m8 stack -> standard");
}

static void
test_remap_semantics(void)
{
    /* expected (BS1, XA1/BS2-line) per enableRead byteSel:
       0=lfuse(0,0) 1=lock(1,0) 2=efuse(0,1) 3=hfuse(1,1) */
    static const int exp_bs1[4] = { 0, 1, 0, 1 };
    static const int exp_bs2[4] = { 0, 0, 1, 1 };
    int sel, ok;

    hvpp_remap_mode = HVPP_REMAP_AUTO;
    hvpp_set_stack(stack_t261);
    CHECK(hvpp_family == HVPP_FAMILY_X61, "set_stack(t261): family = x61");

    ok = 1;
    for (sel = 0; sel < 4; ++sel) {
        uint8_t e = hvpp_stack[20 + sel];   /* enableRead */
        if (BIT(e, 2) != 0) ok = 0;         /* OE active (low) */
        if (BIT(e, 3) != 1) ok = 0;         /* WR inactive */
        if (BIT(e, 4) != exp_bs1[sel]) ok = 0;
        if (BIT(e, 6) != exp_bs2[sel]) ok = 0;   /* BS2 rides XA1 line */
    }
    CHECK(ok, "remap(t261): enableRead OE/WR/BS1/BS2 per byteSel");

    ok = 1;
    for (sel = 0; sel < 4; ++sel) {
        uint8_t d = hvpp_stack[12 + sel];   /* done */
        uint8_t c = hvpp_stack[16 + sel];   /* commit */
        if (BIT(d, 2) != 1 || BIT(d, 3) != 1) ok = 0;  /* idle: OE,WR high */
        if (BIT(c, 3) != 0) ok = 0;                     /* commit pulses WR */
        if (BIT(c, 2) != 1) ok = 0;                     /* OE stays inactive */
    }
    CHECK(ok, "remap(t261): done idle, commit pulses WR on CTRL3");

    /* loadCommand: XA=10; loadAddr high: XA=00 + BS1 */
    CHECK(BIT(hvpp_stack[8], 6) == 1 && BIT(hvpp_stack[8], 5) == 0,
          "remap(t261): loadCommand has XA=10");
    CHECK(BIT(hvpp_stack[1], 4) == 1 && BIT(hvpp_stack[1], 5) == 0 &&
          BIT(hvpp_stack[1], 6) == 0,
          "remap(t261): loadAddr high has BS1=1, XA=00");

    /* commit byteSel for fuse writes: 1 -> hfuse (BS1), 2 -> efuse (BS2) */
    CHECK(BIT(hvpp_stack[17], 4) == 1 && BIT(hvpp_stack[17], 6) == 0,
          "remap(t261): commit[1] selects hfuse");
    CHECK(BIT(hvpp_stack[18], 4) == 0 && BIT(hvpp_stack[18], 6) == 1,
          "remap(t261): commit[2] selects efuse");

    /* standard stacks must pass through untouched */
    hvpp_set_stack(stack_m8);
    CHECK(hvpp_family == HVPP_FAMILY_STD &&
          memcmp((void *)hvpp_stack, stack_m8, 32) == 0,
          "set_stack(m8): identity (no remap)");
    hvpp_set_stack(stack_t2313);
    CHECK(hvpp_family == HVPP_FAMILY_STD &&
          memcmp((void *)hvpp_stack, stack_t2313, 32) == 0,
          "set_stack(t2313): identity (no remap)");

    /* forced raw mode leaves even an x61 stack alone */
    hvpp_remap_mode = HVPP_REMAP_RAW;
    hvpp_set_stack(stack_t261);
    CHECK(memcmp((void *)hvpp_stack, stack_t261, 32) == 0,
          "remap_mode=RAW: t261 stack passed through");
    hvpp_remap_mode = HVPP_REMAP_AUTO;
}

/* ---- framing round-trip ---- */

static uint8_t resp[600];
static size_t resp_len;

static void
send_frame(uint8_t seq, const uint8_t *body, uint16_t len)
{
    uint8_t ck = 0;
    uint16_t i;
    uint8_t hdr[5] = { MESSAGE_START, seq,
                       (uint8_t)(len >> 8), (uint8_t)len, TOKEN };

    for (i = 0; i < 5; ++i) { ck ^= hdr[i]; stk500v2_rx(hdr[i]); }
    for (i = 0; i < len; ++i) { ck ^= body[i]; stk500v2_rx(body[i]); }
    stk500v2_rx(ck);
    resp_len = mock_take_tx(resp, sizeof resp);
}

static int
resp_valid(uint8_t seq)
{
    uint8_t ck = 0;
    size_t i;
    if (resp_len < 7 || resp[0] != MESSAGE_START || resp[1] != seq ||
        resp[4] != TOKEN)
        return 0;
    for (i = 0; i < resp_len; ++i)
        ck ^= resp[i];
    return ck == 0;    /* xor over frame incl. checksum byte is 0 */
}

static void
test_framing(void)
{
    static const uint8_t sign_on[] = { CMD_SIGN_ON };
    uint8_t get_remap[] = { CMD_GET_PARAMETER, PARAM_REMAP_MODE };
    uint8_t set_stack[33];

    send_frame(3, sign_on, 1);
    CHECK(resp_valid(3) && resp[5] == CMD_SIGN_ON &&
          resp[6] == STATUS_CMD_OK && resp[7] == 8 &&
          memcmp(&resp[8], "STK500_2", 8) == 0,
          "framing: sign-on answers STK500_2, checksum valid");

    set_stack[0] = CMD_SET_CONTROL_STACK;
    memcpy(&set_stack[1], stack_t261, 32);
    send_frame(4, set_stack, 33);
    CHECK(resp_valid(4) && resp[6] == STATUS_CMD_OK &&
          hvpp_family == HVPP_FAMILY_X61,
          "framing: SET_CONTROL_STACK accepted, auto-remap fired");

    send_frame(5, get_remap, 2);
    CHECK(resp_valid(5) && resp[6] == STATUS_CMD_OK &&
          resp[7] == HVPP_REMAP_AUTO,
          "framing: GET_PARAMETER(REMAP_MODE)");

    /* corrupt checksum must yield ANSWER_CKSUM_ERROR, then resync */
    stk500v2_rx(MESSAGE_START); stk500v2_rx(6); stk500v2_rx(0);
    stk500v2_rx(1); stk500v2_rx(TOKEN); stk500v2_rx(CMD_SIGN_ON);
    stk500v2_rx(0x55);    /* wrong checksum */
    resp_len = mock_take_tx(resp, sizeof resp);
    CHECK(resp_len >= 7 && resp[5] == ANSWER_CKSUM_ERROR &&
          resp[6] == STATUS_CKSUM_ERROR,
          "framing: checksum error answered");
    send_frame(7, sign_on, 1);
    CHECK(resp_valid(7) && resp[6] == STATUS_CMD_OK,
          "framing: parser resyncs after bad frame");
}

/* ---- escape-token gating: match between frames only ---- */

extern int mock_isp_entries;

static void
test_token_gating(void)
{
    /* unknown command byte, then the full re-entry token as frame data */
    static const uint8_t token_body[] = "?@STC8ISP!";    /* 10 bytes + NUL */
    static const uint8_t sign_on[] = { CMD_SIGN_ON };
    const char *p;

    mock_isp_entries = 0;
    send_frame(20, token_body, 10);
    CHECK(resp_valid(20) && resp[6] == STATUS_CMD_UNKNOWN &&
          mock_isp_entries == 0,
          "token: @STC8ISP! inside a frame body does not reboot");
    CHECK(stk500v2_idle(), "token: parser reports idle between frames");

    /* a frame interrupting a partial match must reset the scanner */
    for (p = "@STC8IS"; *p; ++p)
        stk500v2_rx((uint8_t)*p);
    send_frame(21, sign_on, 1);
    stk500v2_rx('P'); stk500v2_rx('!');
    CHECK(mock_isp_entries == 0,
          "token: frame interrupts a partial token match");

    for (p = "@STC8ISP!"; *p; ++p)
        stk500v2_rx((uint8_t)*p);
    CHECK(mock_isp_entries == 1,
          "token: contiguous token between frames still fires");
}

/* ---- ISP over the socket wiring (mock t461a profile) ---- */

static void
test_isp(void)
{
    static const uint8_t enter[12] = {
        CMD_ENTER_PROGMODE_ISP, 200, 100, 25, 32, 0, 0x53, 3,
        0xAC, 0x53, 0x00, 0x00
    };
    static const uint8_t rdsig[6] = {
        CMD_READ_SIGNATURE_ISP, 4, 0x30, 0x00, 0x01, 0x00
    };    /* RetAddr=4: 1-based, as avrdude sends */
    static const uint8_t multi_lfuse[8] = {
        CMD_SPI_MULTI, 4, 4, 0, 0x50, 0x00, 0x00, 0x00
    };
    static const uint8_t rdhfuse[6] = {
        CMD_READ_FUSE_ISP, 4, 0x58, 0x08, 0x00, 0x00
    };
    static const uint8_t addr0[5] = { CMD_LOAD_ADDRESS, 0, 0, 0, 0 };
    static const uint8_t rdflash[4] = { CMD_READ_FLASH_ISP, 0, 8, 0x20 };
    static const uint8_t rdee[4]    = { CMD_READ_EEPROM_ISP, 0, 4, 0xA0 };
    static const uint8_t leave[3]   = { CMD_LEAVE_PROGMODE_ISP, 1, 1 };
    /* mock memory patterns: flash a^(a>>8), eeprom (a^0xA5)+(a>>8) */
    static const uint8_t exp_flash[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const uint8_t exp_ee[4] = { 0xA5, 0xA4, 0xA7, 0xA6 };

    send_frame(8, enter, 12);
    CHECK(resp_valid(8) && resp[6] == STATUS_CMD_OK,
          "isp: enter progmode (power-cycle entry, AC 53 echo)");

    send_frame(9, rdsig, 6);
    CHECK(resp_valid(9) && resp[6] == STATUS_CMD_OK && resp[7] == 0x92,
          "isp: READ_SIGNATURE_ISP byte 1 = 0x92");

    send_frame(10, multi_lfuse, 8);
    CHECK(resp_valid(10) && resp[6] == STATUS_CMD_OK &&
          resp[10] == 0x62 && resp[11] == STATUS_CMD_OK,
          "isp: SPI_MULTI universal read -> lfuse 0x62");

    send_frame(11, rdhfuse, 6);
    CHECK(resp_valid(11) && resp[6] == STATUS_CMD_OK && resp[7] == 0xDF,
          "isp: READ_FUSE_ISP hfuse = 0xDF");

    send_frame(12, addr0, 5);
    send_frame(13, rdflash, 4);
    CHECK(resp_valid(13) && resp[6] == STATUS_CMD_OK &&
          memcmp(&resp[7], exp_flash, 8) == 0 &&
          resp[15] == STATUS_CMD_OK,
          "isp: READ_FLASH_ISP 8 bytes from word 0");

    send_frame(14, addr0, 5);
    send_frame(15, rdee, 4);
    CHECK(resp_valid(15) && resp[6] == STATUS_CMD_OK &&
          memcmp(&resp[7], exp_ee, 4) == 0,
          "isp: READ_EEPROM_ISP 4 bytes from 0");

    send_frame(16, leave, 3);
    CHECK(resp_valid(16) && resp[6] == STATUS_CMD_OK,
          "isp: leave progmode (parked off, RESET at GND)");
}

int
main(void)
{
    test_classifier();
    test_remap_semantics();
    test_framing();
    test_token_gating();
    test_isp();

    if (sim_warnings) {
        printf("FAIL: %d sim warning(s) during tests\n", sim_warnings);
        ++failures;
    }
    printf(failures ? "== %d FAILURE(S) ==\n" : "== ALL TESTS PASSED ==\n",
           failures);
    return failures ? 1 : 0;
}
