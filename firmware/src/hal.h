/*
 * Hardware abstraction used by the portable protocol core (stk500v2.c, hvpp.c).
 * Two implementations:
 *   - src/hal_stc8.c   real board (SDCC, STC8H8K64U, pin map in board.h)
 *   - host/hal_mock.c  simulated programmer + simulated AVR target (gcc)
 *
 * Control bytes passed to hal_ctrl_set() are ALWAYS in the standard STK500
 * CTRLn mapping, i.e. after any x61 remap (see hvpp.c):
 *   bit0=BS2  bit2=/OE  bit3=/WR  bit4=BS1  bit5=XA0  bit6=XA1  bit7=PAGEL
 * (bit1 unused)
 */

#ifndef HAL_H
#define HAL_H

#include <stdint.h>

#ifdef __SDCC
#define XDATA __xdata
#else
#define XDATA
#endif

/* HVPP pins */
void    hal_ctrl_set(uint8_t sigs);        /* drive control lines from std-mapped byte */
void    hal_ctrl_engage(void);             /* control lines to push-pull (enter progmode) */
void    hal_ctrl_release(void);            /* control lines to inputs (leave progmode/idle) */
void    hal_data_mode(uint8_t output);     /* data bus: 1 = drive, 0 = high-Z read */
void    hal_data_set(uint8_t v);
uint8_t hal_data_get(void);
void    hal_xtal_pulse(void);              /* one XTAL1 high pulse */
void    hal_vcc(uint8_t on);               /* target VCC switch */
void    hal_hv_reset(uint8_t apply12v);    /* 1 = 12 V on RESET, 0 = RESET held at GND */
uint8_t hal_rdy_wait(uint8_t timeout_ms);  /* wait RDY/BSY high; 1 = ready, 0 = timeout */

/* ISP (serial programming through the HVPP socket wiring: on straight-wired
 * 20-pin x61 parts the WR/XA0/XA1 header wires land on PB0/PB1/PB2 =
 * MOSI/MISO/SCK, and the HVPP RESET driver's default state IS the active-low
 * reset ISP needs).  Byte-level so the host mock can simulate the target. */
void    hal_isp_connect(void);             /* MISO to input, SCK+MOSI low */
void    hal_isp_disconnect(void);          /* restore HVPP pin roles */
uint8_t hal_isp_xfer(uint8_t b);           /* clock one byte, MSB first */
void    hal_isp_sck_pulse(void);           /* sync-retry pulse (datasheet) */

/* Timing */
void    hal_delay_ms(uint8_t ms);
void    hal_delay_us(uint8_t us);

/* Transport (response direction; RX is fed into stk500v2_rx by the platform) */
void    hal_tx_byte(uint8_t b);
void    hal_tx_flush(void);

/* Target build only: transport hal_tx_byte routes to (0 UART2, 1 USB CDC);
 * the main loop sets it to whichever transport delivered the last RX byte. */
extern uint8_t hal_transport;

/* Misc */
void    hal_led(uint8_t on);
void    hal_enter_isp(void);               /* reboot into STC ROM USB bootloader */

#endif
