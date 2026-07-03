/* Interactive pin-debug console (target-only; see pindbg.c for commands). */

#ifndef PINDBG_H
#define PINDBG_H

#include <stdint.h>

/* Feed a received byte.  Returns 1 if consumed (debug mode active or just
 * entered); 0 means pass the byte on to the STK500v2 parser. */
uint8_t pindbg_rx(uint8_t b);

#endif
