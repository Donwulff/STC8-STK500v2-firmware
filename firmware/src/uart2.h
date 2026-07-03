#ifndef UART2_H
#define UART2_H

#include <stdint.h>

void    uart2_init(void);
uint8_t uart2_rx_ready(void);
uint8_t uart2_rx_get(void);
void    uart2_tx(uint8_t b);

/* SDCC requires the ISR prototype to be visible in the file defining main() */
void    uart2_isr(void) __interrupt(8);

#endif
