# Stock firmware STK600 USB identity (captured 2026-07-02, Windows USB Device Tree Viewer + avrdude 8.1)

Reference for the future "STK600 USB identity" mode: to be accepted by
avrdude `-c stk600*` (and possibly Microchip Studio) over native USB, our
firmware must reproduce these descriptors and parameter answers.

## USB descriptors (byte-exact hexdumps)

Device descriptor (USB 1.1, vendor-specific class, EP0 64 B, bcdDevice 2.00):

    12 01 10 01 FF 00 00 40 EB 03 06 21 00 02 01 02 03 01

Configuration descriptor set (wTotalLength 32: 1 vendor interface, 2 bulk EPs):

    09 02 20 00 01 01 00 C0 FA          config: self-powered, MaxPower 500 mA
    09 04 00 00 02 FF 00 00 00          interface 0, class FF/00/00
    07 05 83 02 40 00 0A                EP 0x83 IN  bulk 64 B
    07 05 02 02 40 00 0A                EP 0x02 OUT bulk 64 B

Strings (langid 0x0409): 1 = "ATMEL", 2 = "STK600", 3 = serial "01488E230124".
Quirk: stock's string descriptors each carry one trailing NUL wchar inside
bLength (e.g. "STK600" bLength 0x10 instead of 0x0E) — harmless, hosts ignore
it; no need to replicate, but don't be surprised diffing against real dumps.

Endpoint scheme matches the real STK600 (and AVRISP mkII lineage): bulk OUT
0x02 / bulk IN 0x83, STK500v2 framing minus the serial 0x1B/seq/cksum layer
(USB transport sends bare messages; see AVR068/079 and avrdude usbdev code).

Windows side: enumerates under WinUSB (Microchip-provided inf, class GUID
{deb97e2c-8b0f-446f-b280-7cfac41c3bd9}); avrdude 8.1 opens it directly.

## avrdude -c stk600pp -vv sign-on / parameters reported by stock fw

    Programmer model      : STK600
    HW version            : 3
    FW Version Controller : 2.47
    FW Version Periphery 1: 2.03
    FW Version Periphery 2: 2.02
    Routing card          : Not present
    Socket card           : Not present
    RC_ID table rev       : 16383
    EC_ID table rev       : 1
    Vtarget               : 5.2 V
    Varef 0               : 3.39 V
    Varef 1               : 2.39 V
    SCK period            : 8.0 us
    Oscillator            : 16.007 MHz

Notes:
- "Routing/Socket card: Not present" is accepted by avrdude without fuss —
  we can report the same and skip the RC/EC ID plumbing.
- Vtarget 5.2 V is reported while the socket rail measures ~4.2 V on the
  bench → stock likely returns canned values, not an ADC reading. Canned
  values are therefore acceptable for compatibility.
- Oscillator 16.007 MHz claim suggests stock implements the STK600
  clock-generator parameter set; correlates with the CLK-header
  recovery-clock question (bench checklist item 6).
- Same session read signature 1E 92 08 correctly via stk600pp (signature
  reads don't involve BS2, consistent with the known aliasing model).
