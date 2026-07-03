/*
 * STK500v2 framing, dispatch and general commands.
 * Frame: MESSAGE_START seq lenH lenL TOKEN body[len] xor-checksum.
 * Portable: SDCC (target) and gcc (host harness).
 */

#include <string.h>
#include "stk500v2.h"
#include "stk_proto.h"
#include "hvpp.h"
#include "isp.h"

XDATA uint8_t stk_body[STK_MAX_BODY + 4];
uint32_t stk_address;

static uint8_t  seq, cksum;
static uint16_t want, got;

enum { ST_START, ST_SEQ, ST_LEN1, ST_LEN2, ST_TOKEN, ST_BODY, ST_CKSUM };
static uint8_t state = ST_START;

/* Stored parameters */
static uint8_t p_reset_polarity = 1;
static uint8_t p_controller_init = 0;

/* ---- ISP bootloader re-entry token -------------------------------------
 * Byte scanner over inter-frame bytes only: send the ASCII string below on
 * the transport (e.g. `echo -n '@STC8ISP!' > /dev/ttyUSBn`) and the board
 * soft-resets into the STC ROM USB bootloader.  Bytes inside an STK500v2
 * frame are never scanned, so the token appearing in programming data
 * (flash payload, control stack) cannot trigger it.  The P3.2 button
 * remains the hardware fallback. */
static const char isp_token[] = "@STC8ISP!";
static uint8_t tok_ix;

static void
token_scan(uint8_t b)
{
    if (b == (uint8_t)isp_token[tok_ix]) {
        if (!isp_token[++tok_ix]) {
            tok_ix = 0;
            hal_enter_isp();
        }
    } else {
        tok_ix = (b == (uint8_t)isp_token[0]) ? 1 : 0;
    }
}

/* ---- response ---- */

static void
send_response(uint16_t len)
{
    uint16_t i;
    uint8_t ck = (uint8_t)(MESSAGE_START ^ TOKEN ^ seq
                           ^ (uint8_t)(len >> 8) ^ (uint8_t)len);

    hal_tx_byte(MESSAGE_START);
    hal_tx_byte(seq);
    hal_tx_byte((uint8_t)(len >> 8));
    hal_tx_byte((uint8_t)len);
    hal_tx_byte(TOKEN);
    for (i = 0; i < len; ++i) {
        ck ^= stk_body[i];
        hal_tx_byte(stk_body[i]);
    }
    hal_tx_byte(ck);
    hal_tx_flush();
}

/* ---- parameters ---- */

static uint8_t
get_param(uint8_t id, uint8_t *v)
{
    switch (id) {
    case PARAM_BUILD_NUMBER_LOW:  *v = 1;    return 1;
    case PARAM_BUILD_NUMBER_HIGH: *v = 0;    return 1;
    case PARAM_HW_VER:            *v = 1;    return 1;
    case PARAM_SW_MAJOR:          *v = 2;    return 1;
    case PARAM_SW_MINOR:          *v = 0x0A; return 1;
    case PARAM_VTARGET:           *v = 50;   return 1;  /* 5.0 V, fixed */
    case PARAM_VADJUST:           *v = 25;   return 1;
    case PARAM_OSC_PSCALE:        *v = 0;    return 1;
    case PARAM_OSC_CMATCH:        *v = 0;    return 1;
    case PARAM_SCK_DURATION:      *v = 1;    return 1;
    case PARAM_TOPCARD_DETECT:    *v = 0xFF; return 1;
    case PARAM_RESET_POLARITY:    *v = p_reset_polarity;  return 1;
    case PARAM_CONTROLLER_INIT:   *v = p_controller_init; return 1;
    case PARAM_REMAP_MODE:        *v = hvpp_remap_mode;   return 1;
    default:                      return 0;
    }
}

static uint8_t
set_param(uint8_t id, uint8_t v)
{
    switch (id) {
    case PARAM_RESET_POLARITY:
        p_reset_polarity = v;
        return 1;
    case PARAM_CONTROLLER_INIT:
        p_controller_init = v;
        return 1;
    case PARAM_REMAP_MODE:
        if (v > HVPP_REMAP_X61)
            return 0;
        hvpp_remap_mode = v;
        hvpp_apply_remap();
        return 1;
    default:
        /* Accept and ignore: avrdude aborts on failed sets of parameters
         * we don't care about (e.g. against newer conf files). */
        return 1;
    }
}

/* ---- dispatch ---- */

static void
dispatch(void)
{
    int16_t len = 2;    /* default response: [cmd, status] */

    hal_led(1);
    switch (stk_body[0]) {
    case CMD_SIGN_ON:
        stk_body[1] = STATUS_CMD_OK;
        stk_body[2] = 8;
        memcpy(&stk_body[3], "STK500_2", 8);
        len = 11;
        break;

    case CMD_GET_PARAMETER: {
        uint8_t v;
        if (get_param(stk_body[1], &v)) {
            stk_body[1] = STATUS_CMD_OK;
            stk_body[2] = v;
            len = 3;
        } else {
            stk_body[1] = STATUS_CMD_FAILED;
        }
        break;
    }

    case CMD_SET_PARAMETER:
        stk_body[1] = set_param(stk_body[1], stk_body[2])
                    ? STATUS_CMD_OK : STATUS_CMD_FAILED;
        break;

    case CMD_LOAD_ADDRESS:
        stk_address = ((uint32_t)stk_body[1] << 24)
                    | ((uint32_t)stk_body[2] << 16)
                    | ((uint32_t)stk_body[3] << 8)
                    |  (uint32_t)stk_body[4];
        stk_body[1] = STATUS_CMD_OK;
        break;

    case CMD_SET_CONTROL_STACK:
        hvpp_set_stack(&stk_body[1]);
        stk_body[1] = STATUS_CMD_OK;
        break;

    case CMD_ENTER_PROGMODE_ISP: len = isp_enter_progmode();   break;
    case CMD_LEAVE_PROGMODE_ISP: len = isp_leave_progmode();   break;
    case CMD_CHIP_ERASE_ISP:     len = isp_chip_erase();       break;
    case CMD_SPI_MULTI:          len = isp_spi_multi();        break;
    case CMD_READ_FUSE_ISP:
    case CMD_READ_LOCK_ISP:
    case CMD_READ_SIGNATURE_ISP:
    case CMD_READ_OSCCAL_ISP:    len = isp_read_byte_cmd();    break;
    case CMD_PROGRAM_FUSE_ISP:
    case CMD_PROGRAM_LOCK_ISP:   len = isp_program_byte_cmd(); break;
    case CMD_READ_FLASH_ISP:     len = isp_read_flash();       break;
    case CMD_READ_EEPROM_ISP:    len = isp_read_eeprom();      break;
    case CMD_PROGRAM_FLASH_ISP:  len = isp_program_flash();    break;
    case CMD_PROGRAM_EEPROM_ISP: len = isp_program_eeprom();   break;

    case CMD_ENTER_PROGMODE_PP:  len = hvpp_enter_progmode();  break;
    case CMD_LEAVE_PROGMODE_PP:  len = hvpp_leave_progmode();  break;
    case CMD_CHIP_ERASE_PP:      len = hvpp_chip_erase();      break;
    case CMD_READ_SIGNATURE_PP:  len = hvpp_read_signature();  break;
    case CMD_READ_OSCCAL_PP:     len = hvpp_read_osccal();     break;
    case CMD_READ_FUSE_PP:       len = hvpp_read_fuse();       break;
    case CMD_PROGRAM_FUSE_PP:    len = hvpp_program_fuse();    break;
    case CMD_READ_LOCK_PP:       len = hvpp_read_lock();       break;
    case CMD_PROGRAM_LOCK_PP:    len = hvpp_program_lock();    break;
    case CMD_READ_FLASH_PP:      len = hvpp_read_flash();      break;
    case CMD_READ_EEPROM_PP:     len = hvpp_read_eeprom();     break;

    default:
        stk_body[1] = STATUS_CMD_UNKNOWN;
        break;
    }
    send_response((uint16_t)len);
    hal_led(0);
}

/* ---- framing parser ---- */

uint8_t
stk500v2_idle(void)
{
    return state == ST_START;
}

void
stk500v2_rx(uint8_t b)
{
    switch (state) {
    case ST_START:
        token_scan(b);    /* inter-frame bytes only; 0x1B resets the match */
        if (b == MESSAGE_START) {
            cksum = b;
            state = ST_SEQ;
        }
        break;
    case ST_SEQ:
        seq = b;
        cksum ^= b;
        state = ST_LEN1;
        break;
    case ST_LEN1:
        want = (uint16_t)b << 8;
        cksum ^= b;
        state = ST_LEN2;
        break;
    case ST_LEN2:
        want |= b;
        cksum ^= b;
        state = (want && want <= STK_MAX_BODY) ? ST_TOKEN : ST_START;
        break;
    case ST_TOKEN:
        if (b == TOKEN) {
            cksum ^= b;
            got = 0;
            state = ST_BODY;
        } else {
            state = ST_START;
        }
        break;
    case ST_BODY:
        cksum ^= b;
        stk_body[got++] = b;
        if (got == want)
            state = ST_CKSUM;
        break;
    case ST_CKSUM:
        state = ST_START;
        if (cksum == b) {
            dispatch();
        } else {
            stk_body[0] = ANSWER_CKSUM_ERROR;
            stk_body[1] = STATUS_CKSUM_ERROR;
            send_response(2);
        }
        break;
    }
}
