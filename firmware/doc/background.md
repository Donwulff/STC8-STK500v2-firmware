# Background — why this firmware exists

The [README](../README.md) covers wiring, building, and flashing. This page
is the longer story: what is wrong with the stock firmware, why it took so
long to pin down, and what this board actually is.

## The wall, brick by brick

If you have one of these cheap AliExpress "4-in-1 STK600 clone" HVPP
programmers and tried to use it on an **ATtiny261/461/861 (tinyX61)** part,
you hit a wall. Here is the map of that wall, so you don't spend the days we
did finding each brick:

1. **The stock firmware on the unit I got ignores the host control stack.**
   avrdude/Studio upload a 32-byte control stack (`CMD_SET_CONTROL_STACK`)
   that encodes each part's HVPP signal choreography — in particular how
   20-pin parts time-share programming functions onto fewer pins (BS2 riding
   the XA1 line, PAGEL riding the BS1 line, etc.). The stock firmware is
   table-driven and discards the uploaded stack, driving a fixed default
   scheme instead. Wide, non-multiplexed parts (the 28/40-pin DIPs the ZIF
   cards are built for) program fine, which is what hides the bug. But every
   20-pin AVR needs the multiplexed control lines, so programming **any**
   20-pin part — not just x61 — would not have worked on the firmware I
   received: the mechanism that drives the multiplexing (the stack) is never
   used. On the x61 the reads come back plausible but aliased; BS2-dependent
   operations are simply wrong.
2. **The tinyX61 family is the case you can't even hand-wire around.**
   Standard 20-pin parts keep each function on its named line, so a lucky
   subset of operations survives name-to-name wiring. x61 *re-pairs*
   functions across the control lines (XA1+BS2 on one target pin, PAGEL+BS1
   on another), so under any static wiring **no** control stack but an
   x61-shaped one can satisfy it — and the stock firmware won't play one.
   The STK600 manual's generic 20-pin wiring is wrong for x61, and the AVR
   Dragon docs hand you a per-part wiring diagram without explaining *why*.
   The one public page that names it is ScratchMonkey's HVPP notes (see the
   README's **References**); this was the part of the puzzle that took the
   longest to see.
3. **No public source or firmware runs on this board as-is.** ScratchMonkey,
   the reference open HVPP implementation, is AVR-based; nothing
   off-the-shelf targets the STC8H MCU inside this clone. So the only way to
   make it honor the stack is to replace the firmware — which is what this
   repo is.

And the backdrop that makes it worth the effort: **classic high-voltage
parallel programming has no current first-party tool.** Per Microchip
support, *no current MPLAB hardware tool supports this programming method*;
the only ones that ever did — STK600 and AVR Dragon — are discontinued
legacy, and modern tools (PICkit, Power Debugger, MPLAB SNAP) speak UPDI,
not classic HVPP. If you need to un-brick a fuse-corrupted classic
ATtiny/ATmega, a working clone like this is one of the few paths left.

## What the hardware gets right

One genuine edge this hardware has: the 12 V that HVPP/HVSP needs is
**generated on the board itself** (a traced MC34063A boost rail), so there's
no external supply to wire up. It still gets applied to a specific pin — but
to a *fixed, tested socket/header position* rather than a flying lead you
clip on by hand, which removes the "clipped onto the wrong pin" error class
of a bare DIY rig. (The original STK500, by contrast, wanted an external
12 V fed in for HV programming.) The clones' marketing "only solution with
integrated HV" is overstated — the discontinued AVR Dragon and STK600 had it
too — but among cheap, currently-buyable tools it's a fair pitch.

The upside of having been forced to reverse-engineer the whole board: the
repo ships the **`@PINDBG!` hardware-forensics toolkit** (clamp-diode decay
probing, an ISP-pin matrix self-scan where the socketed chip names its own
wires, a VCC-switch hunt). Board revisions vary; that toolkit is what makes
re-deriving the pin map on a *different* revision a short, confident job
instead of another multi-day hunt.

## Identifying the hardware / provenance

What *is* this board? It's sold ubiquitously on AliExpress under **STK500,
STK600, and AVR Dragon guises** (a few listings call it "WaveSTK", though
that name leads nowhere identifiable). The clearest attribution found,
quoted from a retail listing:

> "Nanjing Taoxing Electronics developed a high-voltage programmer
> compatible with Atmel's official STK500. It supports high-voltage parallel
> programming, high-voltage serial programming, ISP programming — three
> modes — and a USB-to-TTL serial-port adapter."

Several AliExpress listings credit **"Taoxing,"** but the company has no
findable online presence (not on Alibaba either). The hardware is
straightforward and resembles most no-brand flashers, so it may simply have
become a de-facto common design passed between sellers. **Treat all of this
as informal** — the quote came from a dynamic commercial listing that may
change or vanish, and *your* unit may be a different revision (different STC
part, different wiring). Use the `@PINDBG!` forensics toolkit to verify your
own board rather than trusting any listing.
