/*
 * USB CDC-ACM transport (native STC8H8K64U USB device controller).
 * Ported from STC demo #61 (Keil) to SDCC.  Target-only.
 */

#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

void usb_cdc_init(void);

uint8_t usb_cdc_rx_ready(void);
uint8_t usb_cdc_rx_get(void);

/* Queue a byte (blocks pumping if the ring is full) / kick the IN pump. */
void usb_cdc_tx(uint8_t b);
void usb_cdc_poll(void);

uint8_t usb_cdc_configured(void);

/* SDCC: ISR prototype must be visible in the file defining main() */
void usb_isr(void) __interrupt(25);

#endif
