#ifndef STK500V2_H
#define STK500V2_H

#include <stdint.h>
#include "hal.h"

#define STK_MAX_BODY 275    /* STK500 hardware limit (AVR068) */

extern XDATA uint8_t stk_body[STK_MAX_BODY + 4];
extern uint32_t stk_address;    /* CMD_LOAD_ADDRESS; bit31 = extended addr flag */

/* Feed one received byte into the framing parser; dispatches complete frames
 * and transmits responses via hal_tx_byte/hal_tx_flush. */
void stk500v2_rx(uint8_t b);

/* Nonzero while the parser is between frames.  Escape-token scanners
 * (@STC8ISP!, @PINDBG!) must only match then, never inside frame data. */
uint8_t stk500v2_idle(void);

#endif
