#ifndef HVPP_H
#define HVPP_H

#include <stdint.h>
#include "hal.h"

/*
 * Control-stack playback engine (port of ScratchMonkey SMoHVPP.cpp, BSD).
 *
 * Stack auto-remap: avrdude uploads a 32-byte control stack whose bit
 * positions encode which STK500 CTRLn header pin to drive.  The tinyX61
 * family stack assumes cross-wired headers (CTRL2=WR, 3=XA0, 4=XA1/BS2,
 * 5=PAGEL/BS1, 7=OE, bit6 parked high, bit0 unused).  We fingerprint the
 * stack at upload and permute those bytes into the standard mapping
 * (bit0=BS2, 2=OE, 3=WR, 4=BS1, 5=XA0, 6=XA1, 7=PAGEL) so that straight
 * signal-to-signal wiring works for every part family.  For x61 parts the
 * BS2 and PAGEL headers are simply left unconnected (those functions live
 * on the chip's XA1/BS2 and PAGEL/BS1 shared pins, reached via the XA1 and
 * BS1 headers).
 */

enum {
    HVPP_REMAP_AUTO = 0,    /* fingerprint the uploaded stack (default) */
    HVPP_REMAP_RAW  = 1,    /* never remap (stack used as uploaded) */
    HVPP_REMAP_X61  = 2     /* force the x61 permutation */
};

enum {
    HVPP_FAMILY_STD = 0,
    HVPP_FAMILY_X61 = 1
};

extern XDATA uint8_t hvpp_stack[32];       /* active (possibly remapped) stack */
extern XDATA uint8_t hvpp_stack_raw[32];   /* as uploaded */
extern uint8_t hvpp_remap_mode;
extern uint8_t hvpp_family;                /* result of last classification */

uint8_t hvpp_classify(const uint8_t *cs);
void    hvpp_set_stack(const uint8_t *raw);
void    hvpp_apply_remap(void);

/*
 * Command handlers.  Each parses arguments from stk_body[] (the received
 * message body), performs the operation, fills stk_body[1..] with status
 * and data, and returns the response body length.
 */
int16_t hvpp_enter_progmode(void);
int16_t hvpp_leave_progmode(void);
int16_t hvpp_chip_erase(void);
int16_t hvpp_read_signature(void);
int16_t hvpp_read_osccal(void);
int16_t hvpp_read_fuse(void);
int16_t hvpp_program_fuse(void);
int16_t hvpp_read_lock(void);
int16_t hvpp_program_lock(void);
int16_t hvpp_read_flash(void);
int16_t hvpp_read_eeprom(void);

#endif
