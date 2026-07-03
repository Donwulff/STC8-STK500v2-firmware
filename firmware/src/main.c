/*
 * Replacement firmware for the STK600-clone 4-in-1 AVR HV programmer
 * (STC8H8K64U).  STK500v2 over UART2 header @115200; HVPP engine with
 * control-stack auto-remap so straight signal-to-signal wiring works for
 * all part families, including tinyX61.
 *
 * Recovery: hold the P3.2 button while plugging USB (ROM bootloader,
 * 34bf:1001), or send '@STC8ISP!' on the transport for a soft re-entry.
 */

#include "board.h"
#include "hal.h"
#include "uart2.h"
#include "usb_cdc.h"
#include "stk500v2.h"
#include "adc.h"
#include "pindbg.h"

static void
feed(uint8_t b)
{
    if (!pindbg_rx(b))
        stk500v2_rx(b);
}

void
main(void)
{
    /* Normalize latches while still quasi-bidir.  VCC_EN (P1.4, active
     * LOW) boots high = socket off, and RESET boots held at GND — both
     * fail-safe; these writes just make the state explicit. */
    hal_vcc(0);
    hal_hv_reset(0);
    hal_data_set(0x00);
    PIN_XTAL1 = 0;

    BOARD_GPIO_INIT();
    /* Idle RELEASED: control pins quasi weak-high, data bus hi-Z.  Driving
     * them push-pull high into the unpowered socket phantom-powers a seated
     * AVR through its clamp diodes — bench-observed 2026-07-02 as a bright
     * socket-VCC LED (tens of mA into the pristine t461a).  Full drive is
     * engaged only inside progmode sessions (hal_ctrl_engage). */
    hal_ctrl_release();
    hal_data_mode(0);

    uart2_init();
    usb_cdc_init();
    adc_init();

    /* boot blink: two short flashes */
    hal_led(1); hal_delay_ms(100); hal_led(0); hal_delay_ms(100);
    hal_led(1); hal_delay_ms(100); hal_led(0);

    while (1) {
        usb_cdc_poll();
        if (usb_cdc_rx_ready()) {
            hal_transport = 1;
            feed(usb_cdc_rx_get());
        }
        if (uart2_rx_ready()) {
            hal_transport = 0;
            feed(uart2_rx_get());
        }
    }
}
