# Third-party notices

This repository is BSD-2-Clause (see `LICENSE`) **except as noted below**.
The policy here is honesty over theater: files with third-party ancestry say
so in their headers and are listed here, rather than being laundered through
a pretend clean-room rewrite.

## ScratchMonkey (BSD) — `firmware/src/hvpp.c`, `firmware/src/hvpp.h`

The HVPP control-stack playback engine is a port of `SMoHVPP.cpp` from
[ScratchMonkey](https://github.com/microtherion/ScratchMonkey),
Copyright (c) 2013-2016 Matthias Neeracher <microtherion@gmail.com>,
licensed under the BSD license per the ScratchMonkey README. The port
rewrites it for SDCC/gcc dual-compilation and adds the x61 stack auto-remap;
the control-stack playback logic and stack-index conventions are his.
ScratchMonkey's BSD terms are the same two clauses and disclaimer as this
repository's `LICENSE`, with his copyright line above.

## STC demo code (no license text) — `firmware/src/usb_cdc.c`, `firmware/src/usb_cdc.h`

The USB CDC-ACM transport is ported from STC demo #61 ("CDC协议范例", Keil
C51) in the `STC8H8K64U-DEMO-CODE` package that STC Micro distributes freely
with their parts. STC ships this code **without any license text**. It is
standard practice in the STC ecosystem to build on it (STC distributes it
precisely so their chips get used), but formally these two files are **not
covered by this repository's BSD license** and no OSI license is claimed for
them. The port is substantially transformed (Keil→SDCC endianness rework,
stripped to CDC-ACM, a documented concurrency model, and fixes for two bugs
present in the original), but its ancestry is what it is. If this matters
for your use: the interface in `usb_cdc.h` is small, and any CDC-ACM stack
for the STC8H USB device core can be dropped in behind it.

## Protocol constants — `firmware/host/test_remap.c`

The three 32-byte HVPP control stacks used as test fixtures are the
`pp_controlstack` values that any STK500v2 host (avrdude, AVR Studio)
transmits for the ATtiny261/461/861, ATtiny2313, and ATmega8. They originate
in Atmel's part-description files and are functional wire-protocol data — an
interoperable implementation necessarily contains these exact bytes. They
were transcribed here via avrdude.conf and are retained as facts, not as
creative content of avrdude.

## Referenced but deliberately NOT included

- **Microchip IEC 60730 Class B Diagnostic Library** — non-OSI
  ("use with Microchip products") license; never vendored. See
  `selftest/README.md`; `selftest/host/fetch_classb_ref.sh` downloads it
  into a gitignored directory for study only. The tests in `selftest/src/`
  are original implementations of public-literature algorithms.
- **STC8H8K64U-DEMO-CODE-V9.6**, **ScratchMonkey**, **avrdude** source
  trees — kept locally as reference material, excluded via `.gitignore`.
