# AVR HVPP programmer — STK600-clone replacement firmware + on-target self-test

Tools built around the cheap AliExpress "4-in-1 STK600 clone" AVR
high-voltage programmer (STC8H8K64U-based), written because the stock
firmware can't actually program the parts the box claims to support.
Published in the spirit of *this may save someone else the frustration*.

Two subprojects:

## [`firmware/`](firmware/README.md) — replacement STC8 firmware

Speaks STK500v2 (`avrdude -c stk500pp` / `-c stk500v2`) with a
part-agnostic HVPP engine that **honors host-uploaded control stacks** —
the thing the stock firmware silently ignores, which is why multiplexed
20-pin parts (ATtiny2313, tinyX61, …) don't work on the stock unit. Also:
ISP mode, tinyX61 crossed-stack auto-remap (wire label-to-label), a
forensics/pin-discovery console (`pindbg`) for verifying *your* board
revision before trusting it, ADC-based rail/current monitoring, and a
pre-flash simulation gate. Read the **one-way-street warning** in the
firmware README before flashing anything: stock firmware cannot be backed
up.

## [`selftest/`](selftest/README.md) — on-target self-test (ATtiny461A)

An IEC 60730 Class B-style diagnostic suite (CPU registers, full-SRAM
March C-, flash CRC, EEPROM, ADC/VCC, watchdog, clock beacon) that runs
**on the AVR itself** and reports back **through the HVPP socket wiring** —
no extra wires, no UART, no test jig — or, for a chip staying on its board,
as an SPI slave on the ISP pins. One command answers "is this chip
actually good?" — incoming inspection of loose parts, qualifying a suspect
chip, or proving code execution before a part goes somewhere hard to
rework. Covers classic AVR parts that Microchip's own Class B library
never supported.

## Quick start

```sh
cd firmware && make gate      # simulate before touching hardware
cd selftest && make run       # flash + run the suite on a socketed t461a
```

Each subproject README has the full howto, wiring tables, and
troubleshooting.

## License & provenance

BSD-2-Clause (see [LICENSE](LICENSE)), with two honestly-labeled
exceptions — the ScratchMonkey-derived HVPP engine (BSD, attributed) and
the STC-demo-derived USB CDC transport (no vendor license text exists) —
detailed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
