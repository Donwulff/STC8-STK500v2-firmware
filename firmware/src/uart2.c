/*
 * UART2 (P1.0 RxD2 / P1.1 TxD2), 115200 8N1, baud from Timer2 in 1T mode.
 * RX interrupt-driven into a ring buffer; TX polled (flag set by ISR).
 */

#include "stc8h.h"
#include "board.h"
#include "uart2.h"

#define BAUD 115200UL
#define T2_RELOAD (65536UL - FOSC / 4UL / BAUD)

#define RX_SIZE 128    /* power of two */

static volatile __xdata uint8_t rx_buf[RX_SIZE];
static volatile uint8_t rx_head, rx_tail;
static volatile __bit tx_done;

void
uart2_init(void)
{
    S2CON = 0x10;                     /* 8-bit UART, RX enabled */
    AUXR &= ~0x18;                    /* Timer2 stop, timer mode */
    AUXR |= 0x04;                     /* Timer2 1T */
    T2H = (uint8_t)(T2_RELOAD >> 8);
    T2L = (uint8_t)T2_RELOAD;
    AUXR |= 0x10;                     /* Timer2 run */
    IE2 |= 0x01;                      /* ES2 */
    EA = 1;
}

uint8_t
uart2_rx_ready(void)
{
    return rx_head != rx_tail;
}

uint8_t
uart2_rx_get(void)
{
    uint8_t b = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) & (RX_SIZE - 1);
    return b;
}

void
uart2_tx(uint8_t b)
{
    tx_done = 0;
    S2BUF = b;
    while (!tx_done)
        ;
}

void
uart2_isr(void) __interrupt(8)
{
    if (S2CON & 0x01) {
        S2CON &= ~0x01;
        rx_buf[rx_head] = S2BUF;
        rx_head = (rx_head + 1) & (RX_SIZE - 1);
    }
    if (S2CON & 0x02) {
        S2CON &= ~0x02;
        tx_done = 1;
    }
}
