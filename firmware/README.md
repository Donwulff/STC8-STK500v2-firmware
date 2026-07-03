# STK600-clone replacement firmware (STC8H8K64U)

Replacement firmware for the commercial 4-in-1 AVR high-voltage programmer.
With the firmware my unit shipped with, **HVPP did not work on 20-pin parts
like the ATtiny461** — every 20-pin AVR multiplexes some programming lines,
the host describes that multiplexing in a control stack it uploads, and the
stock firmware throws the stack away. This firmware plays it faithfully.
(Why the bug hides on 28/40-pin parts, and the rest of the story:
[doc/background.md](doc/background.md).)

Features:

- **Part-agnostic HVPP** — signal choreography comes from the host-uploaded
  control stack (`CMD_SET_CONTROL_STACK`), not a fixed table, so anything
  avrdude has a `pp_controlstack` for is in scope. Bench-validated on
  ATtiny461A; a standard 28/40-pin part is the next validation target.
- **tinyX61 crossed-stack auto-remap** — wire an ATtiny261/461/861
  **label-to-label** like any ordinary 20-pin part instead of the special
  crossed layout the official docs require. The native crossed wiring (the
  Dragon layout, and expansion cards built for it) stays available as a
  selectable raw mode; the firmware plays the stack to match either wiring.
- **ISP through the same HVPP socket** — no rewiring
  (`avrdude -c stk500v2`); flash writes bench-validated.
- **`@PINDBG!` hardware-forensics console** — clamp-diode decay probing, an
  ISP matrix self-scan where the socketed chip names its own wires, VCC
  rail/current monitoring. Board revisions vary; this is how you verify
  *your* unit's wiring before trusting it.
- **Native USB CDC-ACM** transport (UART2 fallback), a button-free reflash
  loop, and a pre-flash simulation gate so you never flash blind.

> ### ⚠️ Reflashing is effectively a ONE-WAY street
> The STC8H's stock firmware **cannot be read out**, so you cannot make a backup
> before you flash. The board stays recoverable *as a board* — the STC mask-ROM
> USB bootloader always runs on cold power-up, so you can always load *some*
> firmware — but restoring the **original** image is only possible if the seller
> gives you one. Some sellers are reportedly helpful and knowledgeable, but don't
> count on it, and "send me your firmware image" is not a small ask. Treat the
> flash as permanent: do it only when you're done using the unit as a stock
> STK600 clone. This project is gated behind a green pre-flash test suite plus an
> explicit go/no-go (see **Pre-flash gate**) for exactly this reason.
>
> **Caveat on everything below:** this describes the *specific unit/revision I
> received*. These clones ship in several board revisions from several sellers;
> yours may differ in firmware behavior, wiring, and even which STC part is
> fitted. The forensics toolkit (below) exists precisely so you can re-verify
> your own board rather than trust mine.

> ### ⚠️ Match the part to the socket before you power it
> The 12 V RESET is applied to whatever pin sits at the RESET position **for the
> pinout the ZIF/wiring assumes.** Put in a chip of a different pin-count, or
> backwards, and that 12 V lands on some random I/O or supply pin of *your* part
> — instant, silent damage. There is currently no software interlock against
> this (see the planned signature gate below). Confirm the part and its
> orientation match the socket/wiring, every time, before applying power.

### Scope / status

**STK500v2 is the target protocol** — avrdude speaks it natively and it
carries everything this hardware can do, so STK600 parameter emulation is a
non-goal. Working today: part-agnostic HVPP with x61 auto-remap, signature /
fuse / lock / flash / EEPROM **reads** and **fuse writes** over HVPP, full
ISP (including flash writes) through the same socket, the `@PINDBG!`
console, native USB-CDC, and the button-free reflash loop. Not done yet:
paged flash/EEPROM **writes over HVPP**, and HVSP (see the milestone list).

## Layout

- `src/` — portable protocol core (`stk500v2.c` framing/dispatch, `hvpp.c`
  control-stack playback + auto-remap; both compile under SDCC and gcc)
  plus target-only code (`board.h` pin map, `stc8h.h`, `uart2.c`,
  `hal_stc8.c`, `main.c`)
- `host/` — mock HAL that simulates the programmer pins AND a straight-wired
  target chip (decodes real OE/WR/XA/BS waveforms), pty harness, unit tests
- `test/run_gate.sh` — the pre-flash gate

## Build / test

    make host && make test    # gcc build + unit tests
    make gate                 # REAL avrdude vs simulated t461a/t2313/m8
    make fw                   # SDCC build -> build/fw.bin

## Pre-flash gate

The first flash **permanently erases the stock firmware**. Requirements
before `make flash CONFIRM=yes`:

1. `make gate` all green (real avrdude signs on, uploads stacks, enters
   progmode, reads signature + all fuses + lock on all three profiles,
   simulator reports zero warnings and zero writes), and
2. an explicit user go/no-go decision.

Flashing: hold the P3.2 button while plugging USB → ROM bootloader
(34bf:1001) → `stc8usb -t 24000000 -f build/fw.bin`.

## Runtime notes

- Primary transport: **native USB CDC-ACM** (STC demo #61 stack ported to
  SDCC) — enumerates 34bf:ff02 "AVR HVPP P…", binds `cdc_acm` on Linux as
  `/dev/ttyACMn`, no driver on Win10+.
  `avrdude -c stk500pp -P /dev/ttyACM0 -p t461a ...`
- Fallback transport: UART2 header (P1.0 RxD / P1.1 TxD), 115200 8N1 —
  both are live simultaneously; responses go to whichever port sent the
  request.
- Sign-on identity: `STK500_2`.
- **Host software:** `avrdude` is the supported/tested host (`-c stk500pp` for
  HVPP, `-c stk500v2` for ISP). **Microchip Studio 7** (the old Atmel Studio —
  needed at all for STK600, and nominally STK500-capable) **does not yet recognize
  this firmware.** Likely an identity/sign-on matter: Studio is pickier than avrdude
  and probably needs the firmware to present as a recognized STK500 (or to carry the
  STK600 USB identity — captured in `doc/stock_stk600_identity.md`). Known
  limitation / TODO; avrdude users are unaffected.
- **Button-free development loop**: `echo -n '@STC8ISP!' > /dev/ttyACM0` →
  board soft-resets into the ROM USB bootloader (34bf:1001) →
  `make flash CONFIRM=yes` → new firmware boots. The P3.2 button is only
  needed for the very first flash over stock, and as recovery if a build
  bricks both transports.
- Remap control: parameter 0x40 via GET/SET_PARAMETER — 0 auto (default),
  1 raw (never remap), 2 force-x61.
- Pin-debug console: send `@PINDBG!` from any terminal (115200) → all GPIO
  go quasi-bidir in the safe state (VCC off, RESET at GND, pullups
  elsewhere). Select a pin (`0`-`7` DATA, `a` XA0, `b` XA1, `c` BS1,
  `d` BS2-dummy/red-LED, `e` OE, `f` WR, `g` PAGEL, `h` XTAL1,
  `r` RESET_DRV, `v` VCC_EN (low = on), `l` ISP-header RST (P1.5),
  `x`/`y` P3.3/P3.4, `z` P3.7, `w` P1.7), then `+`/`-` to drive it;
  `p` prints all port inputs (ground a header pin, see which bit fell);
  `?` help, `B` bootloader, `q` back to STK500v2. No interlocks: `r-` puts
  12 V on the RESET node, `v+` powers the socket. Made for wiring
  verification and ISP-header routing discovery without a multimeter.
- PINDBG hardware-forensics extras (all proven 2026-07-02): `S` = ISP
  MOSIxSCK matrix scan (socketed AVR names its own wires via the 0x53
  Programming-Enable echo), `C` = clamp-decay probe (charge pins, hi-Z,
  watch discharge — distinguishes wired/unwired pins, chip presence, and
  whether the rail is really powered; inherits VCC/RESET state), `V` =
  VCC_EN hunt.
- All board.h polarities/pins are PROVEN, and every P1 mystery is solved
  (the original pin table was from the wrong LQFP32 variant page of the
  STC8H datasheet; true row: pins 1-8 = P1.0, P1.1, P1.4, P1.5, P1.6,
  P1.7, P1.3, UCap): VCC_EN = P1.4 **active low** (PNP 2TY, 1k base),
  WR = P1.3, OE = P1.7, RESET_DRV low = 12 V, P1.5 = ISP/JTAG-header RST
  via 100R (not on the ZIF; PINDBG key 'l'), P1.6 = red LED (1k to 4.2 V,
  low = on; doubles as dummy BS2 → blinks during HVPP sessions).  P1.2 has
  no pad on this package — physical pin 8 is UCap.  Open item: the green
  LED (not on an MCU pin; somewhere in the switched-VCC network).
- **ISP works through the HVPP socket** with no rewiring: WR/XA0/XA1
  headers are MOSI/MISO/SCK on x61 parts, RESET held at GND is the ISP
  reset, VCC is switched.  `avrdude -c stk500v2 -P /dev/ttyACM0 -p t461a`.
  Note: a DWEN-fused part has ISP disabled → use HVPP for it.
- The socket rail wobbles at light load (dips at 10-100 ms after switch-on,
  varies per run); ISP entry sweeps power-on phase + burst-retries with
  SCK sync pulses to ride it out.

## ATtiny461A straight wiring (PDIP20) — milestone 2/3 reference

The ZIF cards these boards ship with are **standard wide-DIP sockets (28/40-pin
class)**, and a 20-pin part **cannot** use them — not just because of the signal
pins, but because **RESET (the 12 V HV pin) is hardwired to the 28/40-pin RESET
position** and that routing is fixed in copper, not configurable in firmware. On
a 20-pin part that physical position is a *different* pin, so seating an x61 in
the ZIF would put 12 V on the wrong pin. There is no way around this: **20-pin
parts must be wired to the HVPP header on a separate socket** (a plain DIP
socket, or an adapter for SMD packages). This is a hard limitation, not a
to-do.

> **Wire the x61 the STANDARD way — do NOT follow ScratchMonkey's x61 wiring
> table.** This firmware detects the x61 control stack and remaps it on the fly,
> so **you wire an x61 exactly like any ordinary 20-pin part** (the label-to-label
> table below, PAGEL/BS2 open). ScratchMonkey and the Atmel/Dragon docs instead
> show the *native x61 crossed* wiring, which pairs with an un-remapped stack — if
> you wire that way here (in the default auto-remap mode) programming just fails
> (it won't fry anything). If you specifically want the crossed wiring — e.g. to
> use an **existing expansion card built for the official x61 layout** — switch
> the remap parameter to **raw** (see below) so the stack is played unmodified.
> Supporting those cards is a real reason raw mode exists, not an afterthought.

Name-to-name, the standard 20-pin convention. **PAGEL and BS2 headers stay
open** — on tinyX61 those functions normally ride the XA1 and BS1 lines; the
firmware's auto-remap is what lets you keep this ordinary wiring instead of the
crossed x61 layout.

| Header | t461a pin | | Header | t461a pin |
|--------|-----------|-|--------|-----------|
| WR     | 1 (PB0)  | | DATA0  | 20 (PA0) |
| XA0    | 2 (PB1)  | | DATA1  | 19 (PA1) |
| XA1    | 3 (PB2)  | | DATA2  | 18 (PA2) |
| BS1    | 4 (PB3)  | | DATA3  | 17 (PA3) |
| VCC    | 5 + 15 (VCC+AVCC) | | DATA4 | 14 (PA4) |
| GND    | 6 + 16 (GND+AGND) | | DATA5 | 13 (PA5) |
| XTAL1 ("CLK") | 7 (PB4) | | DATA6 | 12 (PA6) |
| OE     | 8 (PB5)  | | DATA7  | 11 (PA7) |
| RDY    | 9 (PB6)  | | PAGEL  | — open   |
| RESET  | 10 (PB7) | | BS2    | — open   |

Same wiring is valid under stock firmware too (where BS2-dependent reads
alias, the known stock bug) — no rewiring needed across the flash.

## Milestone status

- [x] Scaffold + host harness + unit tests (26 tests incl. ISP)
- [x] Pre-flash gate green (2026-07-02, avrdude, t461a/t2313/m8)
- [x] USB CDC-ACM transport + `@PINDBG!` console — live on hardware
- [x] First flash 2026-07-02 (stock gone); soft-reflash loop proven
- [x] Milestone 1: bring-up + ALL pin/polarity mysteries solved remotely
      (clamp-diode forensics, 2026-07-02)
- [x] Milestone 2 (2026-07-02, EXCEEDED): pristine t461a straight-wired,
      sig 1E 92 08 + lfuse 0x62 / hfuse 0xDF / efuse 0xFF / lock 0xFF,
      no -F, via **both** `-c stk500pp` (HVPP) and `-c stk500v2` (ISP);
      full flash + EEPROM read validated (factory blank)
- [x] Milestone 3 (2026-07-02): read-only HVPP forensic snapshot of a
      fault specimen validated end to end (signature + all fuses + lock),
      cross-checked on a second host
- [x] ISP flash writes + HVPP fuse writes bench-validated (2026-07-03,
      via the selftest flash/run/restore flow)
- [ ] Milestone 4: regression on a standard 28/40-pin part
- [ ] Paged flash/EEPROM writes over HVPP; HVSP

## Planned safety interlocks & auto-wiring (TODO)

Ideas not yet built, roughly in order of value:

- **Wiring-convention selection — partly done.** The remap already *is*
  selectable at runtime via parameter `0x40` (0 = auto-detect x61 and remap so you
  wire label-to-label, 1 = raw/never-remap for native crossed wiring, 2 =
  force-x61). What's missing is making it convenient and, ultimately, unnecessary:
  auto-probing the actual wiring (below) so the firmware picks the mode from what
  it measures instead of what it's told.
- **Signature gate before HV.** Read the device signature over **ISP first**
  (ISP needs no 12 V — just RESET at ground), and only raise the 12 V RESET if it
  matches the selected part. This fails safe on a misinserted or wrong-pin-count
  chip: no valid signature ⇒ no high voltage. Feasible here precisely because ISP
  runs through the same socket. Needs a **`force`** path for parts whose fuses
  disable ISP (DWEN / RSTDISBL / SPIEN cleared), where HVPP is the *only* way in
  and a pre-check is impossible — so this is a CLI/avrdude-side nicety (Microchip
  Studio exposes no "force", so it can't offer the gate either way).
- **Orientation sanity check via clamp scan.** The RESET pin (and the supply
  pins) differ from ordinary I/O in on-die clamp structure — RESET tolerates high
  voltage and lacks the usual upper clamp to VCC. Probing the expected-RESET
  position for the *absence* of that clamp confirms a correctly-oriented part is
  actually seated before any 12 V is applied. Extends the existing `@PINDBG!`
  clamp-diode forensics.
- **Auto-locate the wiring (`@WIREDOC`).** Hand-wiring a dozen control/data
  lines to a socket, it's genuinely easy to swap two of them — so let the board
  find them. As long as the **ISP wires are right and an ISP-enabled chip is
  seated**, the matrix scan already identifies MOSI/SCK/MISO (the chip echoes
  `0x53`); from there, flash a tiny AVR test program that walks each port pin in a
  known sequence while the programmer watches all of its own header inputs —
  building the full header↔AVR-pin map automatically. Then only **VCC, GND, RESET,
  and a way in** must be wired correctly; every other I/O can be connected in any
  order and the firmware adapts. It sounds absurd ("wire it almost however you
  want, it'll just work") and almost nobody does it, but when the I/Os are
  electrically equivalent there's no fundamental reason it can't — and this board
  can already reflash freely and self-scan, so it's unusually well placed to try.

## Identifying the hardware / provenance

What *is* this board? It's sold ubiquitously on AliExpress under **STK500,
STK600, and AVR Dragon guises** (a few listings call it "WaveSTK", though that
name leads nowhere identifiable). The clearest attribution found, quoted from a
retail listing:

> "Nanjing Taoxing Electronics developed a high-voltage programmer compatible
> with Atmel's official STK500. It supports high-voltage parallel programming,
> high-voltage serial programming, ISP programming — three modes — and a
> USB-to-TTL serial-port adapter."

Several AliExpress listings credit **"Taoxing,"** but the company has no findable
online presence (not on Alibaba either). The hardware is straightforward and
resembles most no-brand flashers, so it may simply have become a de-facto common
design passed between sellers. **Treat all of this as informal** — the quote came
from a dynamic commercial listing that may change or vanish, and *your* unit may
be a different revision (different STC part, different wiring). Use the `@PINDBG!`
forensics toolkit to verify your own board rather than trusting any listing.

## References

- **ScratchMonkey — High Voltage Parallel Programming.** The reference open
  HVPP implementation, and the one public page that actually *names* the x61
  problem: *"on 20 pin MCUs, some of the control signals are multiplexed two to
  a single pin, and the ATtiny26/261/461/861 family does this in a somewhat
  different way from the ATtiny2313/4313 family."* Includes the per-family
  control-signal pin table and points you at `pp_controlstack` in the avrdude
  config. <https://microtherion.github.io/ScratchMonkey/HVPP.html>
  **Caveat for this firmware:** that page's x61 *wiring* is the native crossed
  layout. We solve the same problem the opposite way — remapping the stack so you
  wire label-to-label — so **do not copy its x61 wiring diagram**; use the
  wiring table above instead. It explains the *why*; our wiring is deliberately
  the standard one.
- Microchip support — High Voltage Programming on AVR devices (confirms no
  current MPLAB hardware tool supports classic HVPP):
  <https://support.microchip.com/s/article/High-Voltage-Programming-on-AVR-devices>
- AVR068 (STK500v2 protocol) and AVR079 (STK600 deltas) — the normative
  command/parameter references this firmware implements.

