/*
 * HAL implementation for the real board (SDCC, STC8H8K64U @ 24 MHz 1T).
 */

#include "board.h"
#include "hal.h"
#include "uart2.h"
#include "usb_cdc.h"
#include "adc.h"

uint8_t hal_transport;      /* 0 = UART2, 1 = USB CDC */

void
hal_ctrl_set(uint8_t s)
{
    PIN_BS2   = (s & 0x01) != 0;
    PIN_OE    = (s & 0x04) != 0;
    PIN_WR    = (s & 0x08) != 0;
    PIN_BS1   = (s & 0x10) != 0;
    PIN_XA0   = (s & 0x20) != 0;
    PIN_XA1   = (s & 0x40) != 0;
    PIN_PAGEL = (s & 0x80) != 0;
}

void
hal_ctrl_engage(void)
{
    BOARD_CTRL_ENGAGE();
}

void
hal_ctrl_release(void)
{
    BOARD_CTRL_RELEASE();
}

void
hal_data_mode(uint8_t output)
{
    if (output)
        BOARD_DATA_OUT();
    else
        BOARD_DATA_IN();
}

void
hal_data_set(uint8_t v)
{
    P2 = v;
}

uint8_t
hal_data_get(void)
{
    return P2;
}

void
hal_xtal_pulse(void)
{
    PIN_XTAL1 = 1;
    hal_delay_us(1);
    PIN_XTAL1 = 0;
    hal_delay_us(1);
}

void
hal_vcc(uint8_t on)
{
    PIN_VCC_EN = on ? VCC_ON_LEVEL : !VCC_ON_LEVEL;
    adc_guard_arm(on);      /* variables only — proven VCC->12V timing intact */
}

void
hal_hv_reset(uint8_t apply12v)
{
    PIN_RESET_DRV = apply12v ? !RESET_DRV_GND_LEVEL : RESET_DRV_GND_LEVEL;
}

/* ISP through the socket wiring: MOSI = PIN_WR (P1.6), MISO = PIN_XA0
 * (P3.5, flipped to input), SCK = PIN_XA1 (P0.2).  See isp.c header. */

void
hal_isp_connect(void)
{
    /* Replicate the PINDBG scan state that PROVED sync 2026-07-02:
     * XTAL1 high, data bus quasi-high, MISO weak-pullup input. */
    PIN_XA0 = 1;                    /* latch high, then quasi = weak-pullup */
    P3M0 &= (uint8_t)~0x20;         /*   input: MISO */
    PIN_XA1 = 0;                    /* SCK idle low (required before VCC) */
    PIN_WR  = 0;                    /* MOSI low */
    PIN_XTAL1 = 1;
    P2M1 = 0x00; P2M0 = 0x00; P2 = 0xFF;
    /* SCK/MOSI stay QUASI (strong low, 2-clock strong high burst + weak
     * hold) — exactly how the scan drove them when sync was proven. */
}

void
hal_isp_disconnect(void)
{
    BOARD_CTRL_RELEASE();           /* back to released idle, no drive */
}

uint8_t
hal_isp_xfer(uint8_t b)
{
    /* Mode-0 SPI, MSB first, deliberately slow (~30 us/bit): matches the
     * pace the PINDBG scan proved against the real chip.  hal_delay_us
     * undershoots (~0.5-0.7 us/unit), so units are generous. */
    uint8_t i, r = 0;

    for (i = 0; i < 8; ++i) {
        PIN_WR = (b & 0x80) != 0;
        b <<= 1;
        hal_delay_us(15);
        PIN_XA1 = 1;
        hal_delay_us(12);
        r = (uint8_t)(r << 1) | (PIN_XA0 ? 1 : 0);
        hal_delay_us(4);
        PIN_XA1 = 0;
        hal_delay_us(15);
    }
    hal_delay_us(50);    /* inter-byte gap, matches the proven scan pace */
    return r;
}

void
hal_isp_sck_pulse(void)
{
    PIN_XA1 = 1;
    hal_delay_us(12);
    PIN_XA1 = 0;
}

uint8_t
hal_rdy_wait(uint8_t timeout_ms)
{
    /* poll every ~50 us; +10 ms grace as in ScratchMonkey */
    uint16_t n = ((uint16_t)timeout_ms + 10) * 20;
    while (n--) {
        if (PIN_RDY)
            return 1;
        hal_delay_us(50);
        if ((n & 31) == 0)      /* sag check every ~1.6 ms of waiting */
            adc_guard_check();
    }
    return 0;
}

void
hal_delay_us(uint8_t us)
{
    /* ~24 cycles per iteration at 24 MHz 1T; a bit long is fine */
    while (us--) {
        __asm__("nop"); __asm__("nop"); __asm__("nop"); __asm__("nop");
        __asm__("nop"); __asm__("nop"); __asm__("nop"); __asm__("nop");
    }
}

void
hal_delay_ms(uint8_t ms)
{
    while (ms--) {
        uint8_t i = 4;
        while (i--)
            hal_delay_us(250);
        adc_guard_check();      /* no-op unless target VCC is on */
    }
}

void
hal_tx_byte(uint8_t b)
{
    if (hal_transport)
        usb_cdc_tx(b);
    else
        uart2_tx(b);
}

void
hal_tx_flush(void)
{
    if (hal_transport)
        usb_cdc_poll();
}

void
hal_led(uint8_t on)
{
    PIN_LED = on ? LED_ON_LEVEL : !LED_ON_LEVEL;
}

void
hal_enter_isp(void)
{
    /* First attempt (2026-07-02) proved a bare IAP_CONTR=0x60 does NOT get
     * us to the USB bootloader here: the device stayed enumerated with a
     * dead main loop (wedged in the old while(1)).  Two fixes: detach USB
     * first so the host sees a coherent re-attach of whatever comes next
     * (ISP or app), and if the ROM refuses the soft entry, recover the app
     * instead of hanging until a replug. */
    hal_delay_ms(10);           /* drain the transport */
    IE2 &= (uint8_t)~0x80;      /* EUSB off */
    USBCON = 0x00;              /* release D+ pull-up: host sees detach */
    hal_delay_ms(250);
    hal_delay_ms(250);
    IAP_CONTR = 0x60;           /* SWBS|SWRST: reset, boot from ISP ROM */
    hal_delay_ms(100);
    /* Still executing: no reset happened.  Re-attach as the application. */
    usb_cdc_init();
}
