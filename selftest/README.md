# On-target self-test for classic AVRs (ATtiny461A)

An IEC 60730 Class B-style diagnostic suite that runs **on the AVR itself**
and reports its verdicts back **through the HVPP socket wiring** of the STC8
programmer board — no extra wires, no UART, no test jig. One `make run`
answers "is this chip actually good?": CPU, SRAM, flash, EEPROM, ADC,
watchdog and clock each get a real functional test, executed by the chip
under test. Typical uses: incoming inspection of loose or recycled parts,
qualifying a suspect chip during fault analysis, or proving a part executes
code before it goes somewhere hard to rework.

The companion STC8 firmware (`../firmware`) is unmodified by this suite; it
only needs the pindbg FA build (commands `p`, `h`, `W`, `v`) from 2026-07-03
or later.

## Feature list

| # | Test | Source | Notes |
|---|------|--------|-------|
| 1 | CPU register walk | `src/cpu_regs.S` | r0–r31 + SREG + SPL/SPH, 0xAA/0x55 checkerboard |
| 2 | SRAM March C- | `src/march.S` | **all 256 bytes** — runs at `.init3` before the C runtime claims RAM |
| 3 | Flash CRC | `src/flash_crc.c` | CRC-16/CCITT self-read via LPM; reference patched into last 2 bytes at build |
| 4 | EEPROM | `src/eeprom_test.c` | AA/55/FF full array, **destructive**, leaves it erased |
| 5 | ADC / VCC | `src/adc_check.c` | internal 1.1 V bandgap (MUX5:0=011110) → VCC in mV |
| 6 | Watchdog | `src/main.c` | arms 250 ms WDT, verifies the reset actually happens; results survive in `.noinit` |
| 7 | Clock beacon | `src/main.c` | 62.5 Hz square wave on the XTAL1 wire at all times → calibrated-RC accuracy via STC ADC capture |

Every test is a compile-time switch (`src/config.h`):

```
make FEATURES='-DCFG_TEST_EEPROM=0 -DCFG_TEST_WDT=0'
```

## Wiring / reporting map (straight x61 HVPP wiring)

Chip-pin roles per datasheet 2588F Table 18-12, bench-verified 2026-07-03
(the x61 re-pairs signals: WR is PB0, OE is PB5, RDY/BSY is PB6):

| AVR | x61 HVPP role | header → STC pin | readout |
|-----|---------------|------------------|---------|
| PA0–7 | DATA bus | DATA → P2 | pindbg `p` → `P2=xx` result byte |
| PB0 | WR | WR → **P1.3** | frame strobe, `p` → P1 bit 3 |
| PB4 | XTAL1 | XTAL1 → P0.0 = ADC ch8 | clock beacon, `h` + `W` capture |
| PB1/PB2/PB3/PB5/PB6 | XA0/XA1/BS1/OE/RDY | P3.5/P0.2/P3.6/P1.7/P0.1 | untouched inputs |
| PB7 | RESET | RESET header | **see below** |

### ⚠ True power-on reset (mandatory under RSTDISBL)

With RSTDISBL, POR is the only reset the chip has — and POR only re-arms
if VCC actually falls to ~0. The STC's quasi-high pins **backfeed the
socket rail through the chip's clamp diodes** (the board's known
phantom-power physics), so a plain `v` `+`/`-` cycle can trap a
browned-out first boot *forever* (bench-diagnosed 2026-07-03: rail
wobble at switch-on + BOD disabled + backfeed = chip silent until a real
POR). The script's `power_up()` does the full dance: VCC off → drive
every socket-wired pin low → wait ~2.5 s → VCC on → release pins.
Manual equivalent: `v` `+`, then `-` on `0`–`7 a b c e f g h`, wait,
`v` `-`, then `+` on the same pins.

### ⚠ The RESET jumper (the one manual step)

The HVPP RESET node is a **0 V / 12 V-only driver**: an NPN holds it at GND
by default (fail-safe), and driving it means +12 V. There is no
"released, chip runs" state — a chip wired to that header sits in
permanent reset (this is by design; `firmware/src/isp.c` documents it).
So the flow is: **flash with the RESET wire in place, then move its
chip-side end** before collecting results (`make run` prompts at the
right moment):

- **Option A — unplug it.** PB7 floats, the internal ~30–60 kΩ pull-up
  takes over, power-on reset fires when `v` `-` raises VCC. Zero effort;
  replug to reflash.
- **Option B — move it to the ISP-header RST pin (P1.5, 5 V GPIO via
  100 Ω), recommended for wire-movers.** Firmware keeps P1.5 quasi
  weak-high = run; pindbg `l` `-`/`l` `+` gives clean 5 V-level reset
  control — which is exactly what the reset-line current experiment below
  needs. Bonus: P1.5 is STC ADC ch5, so `l` + `A`/`W` read the actual
  RESET pin voltage. Reflashing still requires the wire back on the HVPP
  header (ISP entry relies on reset-at-GND) until the firmware learns to
  assert P1.5 during ISP.
- **Option C — `--rstdisbl`: no wire touched at all.** RSTDISBL turns PB7
  into a GPIO that boots Hi-Z, so the chip ignores the GND-held node and
  runs on power-on reset. The suite never drives PB7, so it runs
  identically. Safety order is built in: the script first **proves the
  HVPP recovery path on this very chip** and aborts before touching
  anything if any check fails. "Read then write the same value" is not
  trusted — an aliased read would make it write a wrong value to the
  real byte — so the pre-flight is: (1) all four fuse bytes read via
  **both** ISP and HVPP must agree (disjoint pins and mechanisms);
  (2) the lock byte's hardwired-1 bits 7:2 as the independent aliasing
  telltale; (3) the HVPP **write** path proven end-to-end by toggling
  the benign SELFPRGEN bit (efuse bit 0), verifying through ISP, and
  restoring — because read health never implies write health (reads
  pulse OE, writes pulse WR through different byte-select lines). Only
  then does it flip RSTDISBL — read-modify-write, **only bit 7
  changes**, whatever else is in hfuse is preserved. **ISP is dead while the fuse is set** — `--restore-reset`
  sets bit 7 back (again RMW, over HVPP, immune to RSTDISBL — the raison
  d'être of this programmer) and proves ISP is back with a read-only
  probe. Don't put a chip back into service without the restore step. An explicit
  `--factory-fuses` (0x62/0xDF/0xFF via HVPP) exists for when you *want*
  defaults; nothing else assumes them.

Current-consumption facts for interpreting `M` readings around RESET:
the board's NPN burns ~12 mA from the always-on 12 V rail whenever it
holds the node low (constant, part of the baseline); the chip's own cost
of RESET-at-GND is just its internal ~30–60 kΩ pull-up, ~70–140 µA from
target VCC — under RSTDISBL even that disappears (GPIO pull-up off at
boot). Both are below the sag guard's ~10–20 mA floor.

## Reading the results

The chip reports by looping a sequence of **nine frames** forever. For each
frame it puts one byte on the PA bus (visible as `P2=xx` in pindbg), raises
the strobe line for ~300 ms, then lowers it for ~300 ms — slow enough to
follow by eye with repeated `p` commands; the host script decodes it
automatically. The first frame is always the sync byte `0xA5`, and the
*position* of each frame after it says what the byte means:

| frame | meaning | values |
|-------|---------|--------|
| 0 | sync | always `0xA5` |
| 1–6 | verdicts: CPU regs, SRAM march, flash CRC, EEPROM, ADC/VCC, watchdog | high nibble = test number; low nibble: `A` pass, `F` fail, `0` skipped/disabled |
| 7 | VCC as measured by the chip | mV ÷ 32 (`0x6A` = 106 → ~3.4 V; `0` if ADC test off) |
| 8 | MCUSR of the tested boot | `0x01` = clean power-on reset |

The watchdog verdict is special: `0x6A` means "the WDT reset we armed
really happened", `0x60` means it hasn't fired yet (first pass through),
and `0x6E` flags a watchdog reset nobody armed. A fully healthy chip
reads:

```
A5 1A 2A 3A 4A 5A 6A 6A 01
```

— six passes, VCC ≈ 3.4 V, clean power-on boot.

## In-circuit readout over SPI (ISP pins)

For a chip that stays on its board, the suite can serve the same results as
an **SPI mode-0 slave** instead of driving the HVPP report bus. On the x61
the USI's three-wire pins are exactly the ISP pins (DI=PB0/MOSI, DO=PB1/MISO,
USCK=PB2/SCK), so any SPI master already sharing the ISP bus — a host MCU on
the same PCB, or the programmer itself — can clock the results out with no
extra wiring. Not yet bench-validated; the HVPP report path is.

```sh
make FEATURES='-DCFG_REPORT_USI=1 -DCFG_USI_CS_PORT=PORTA \
               -DCFG_USI_CS_PIN=PINA -DCFG_USI_CS_BIT=3'   # CS pin = yours
```

The slave streams **10 bytes, repeating**: the nine frames above, then a
check byte chosen so the 8-bit sum of all ten is zero. MOSI is ignored, so
it doesn't matter what the master transmits. Chip select (active low,
internal pullup, any spare pin — set the three `CFG_USI_CS_*` defines to
match your board) makes the read deterministic: DO is released while
deselected and the stream restarts at `0xA5` on each assertion, so *assert
CS, clock 10 bytes, verify the sum* is the whole transaction. Without the
CS defines the slave drives DO continuously and free-runs — fine on the
bench, never on a shared bus. Either way the slave resyncs on a stalled
clock: if SCK sits idle mid-byte for over ~10 ms the stream resets to the
sync byte, so a CS-less master can still get a framed read (idle the clock
20 ms, then clock 10 bytes), and a stray clock edge can't misalign the
stream for good.

The programmer can read the stream itself: pindbg key `R` masters SCK on
the XA1 header and samples MISO on XA0 (where straight wiring puts the
chip's USI), printing the raw bytes plus the checksum-verified frame.

Practical notes for in-circuit use:

- The USI build drives **nothing but DO while selected** (PA0–7 and the
  strobe stay inputs; the PB4 beacon is off by default in this mode), so it
  won't fight other nets on the board.
- Byte reloads are polled: keep SCK at or below ~50 kHz at the default
  1 MHz core clock, with a small gap between bytes. A torn byte fails the
  check sum — just read the frame again.
- The suite **replaces application flash**, and the EEPROM test **erases
  the EEPROM** — build with `-DCFG_TEST_EEPROM=0` if the board's EEPROM
  contents matter.

## How to run

```sh
make                # build build/selftest.hex (needs avr-gcc, python3)
make run            # flash via the board's ISP path + decode results
make run PORT=/dev/ttyACM1
# or step by step:
make flash
python3 host/run_selftest.py --no-flash --beacon
# fuse route (Option C): flash + RSTDISBL, run in place, then restore:
python3 host/run_selftest.py --rstdisbl --beacon
python3 host/run_selftest.py --restore-reset
```

`make run` drives the full sequence: kills stray port readers, flashes with
avrdude (`-c stk500v2 -p t461a` — the board's own ISP mode), enters pindbg,
powers the socket, collects one frame round, prints PASS/FAIL per test.
`--beacon` additionally captures the clock beacon and estimates F_CPU.
`--off` powers the socket back down to safe idle and exits.

`python3 host/fingerprint.py` takes a passive pin fingerprint of whatever
is in the socket (port snapshot, VCC, per-pin ADC float levels, decay
probe) in a fixed order, so runs are diffable against a known-good part.
Instant decay (0–1 ticks) on a wired pin is the robust leak signature;
absolute decay times and float levels vary with residual phantom-rail
charge and the specimen's fuse state, so compare like with like.

Expected on a healthy chip (bench-verified 2026-07-03): everything
PASS; VCC ≈ 3300–3600 mV as measured *by the
chip* (the STC rail ~4.1 V minus the PNP switch drop — the AVR sits
behind it); MCUSR = PORF (frame 8 reports the *tested* boot, which was
the power-on one; the WDT reset that follows is what frame 6 confirms).
Beacon: the raw Hz uses the STC's nominal ~1 ms tick, which actually runs
~1.33 ms — the trustworthy F_CPU check is the *frame period*: 608 ms
nominal, and wall-clock spacing within ~1 % of that means the RC clock is
healthy (the beacon number then just calibrates the STC tick).

Manual poke, if you prefer a terminal: `@PINDBG!`, then `v` `-` to power the
socket, then repeat `p` and watch `P2=` while P1 bit 3 (the strobe) toggles.

### Troubleshooting

- **No frames, P2=FF:** the chip is in reset — the RESET wire is still on
  the HVPP header (see above). The script prints this diagnosis after ~5 s.
- **No frames otherwise:** check `p` — P1 bit 3 should toggle every
  ~300 ms. If P2 is stuck at FF the chip isn't driving PA (not running,
  or not flashed).
- **Garbled serial:** one reader only. `fuser -k /dev/ttyACM0` first, and
  give ModemManager ~10 s after replugging before touching the port.
- **avrdude can't sync:** a previous session may have left pindbg active —
  it eats all bytes. Send `q` first (the script does this).

## What the STC can (and can't) see on the RESET net

The P5.4/ch2 divider reading (~2.45 V) watches the NPN's **base** network
only — the collector (the actual RESET node) is not on any STC pin, so a
chip driving its ex-RESET pin (PB7 under RSTDISBL) is invisible to ch2.
It is NOT invisible overall, though:

- **Current:** PB7 driven high against the saturated NPN is a near-short —
  ~35 mA from target VCC, comfortably above the sag guard's floor. `M`
  would show it.
- **Voltage:** with the wire on the ISP-header RST pin (Option B), PB7
  lands on P1.5 = ADC ch5 — `l` + `A`/`W` read the pin directly, and a
  drive fight against the STC's own `l` `-` shows up as a divider between
  the two drivers through the 100 Ω.

`FEATURES='-DCFG_DRIVE_RESET_HIGH=1'` builds an image that deliberately
drives PB7 high in the report loop (needs RSTDISBL). **This creates a
deliberate drive fight** (~35 mA into a hard short): prefer Option B's
100 Ω path, keep sessions short, sacrificial chips only. Why you'd want
it: if fuse corruption is suspected of setting RSTDISBL in a fielded
device, firmware that also drives the ex-RESET pin will fight whatever
pulls the board's reset net low — this image lets you characterize that
current signature safely on the bench before going looking for it.

## Reset-line current experiment (manual, needs Option B wiring)

With the reset wire on the ISP-header RST pin and the suite in its report
loop: `l` `-` pulls RESET low while watching `M` (VCC mV + sag guard).
This measures supply current with RESET held low while the core *was*
running — useful when chasing unexplained current on a reset net, and
something a halted-core bench test can't show. Release with `l` `+`; the
chip reruns the whole suite on the external reset (EXTRF), which is
itself a bonus reset-recovery test. (Under Option A wiring there's no
reset control — power-cycle with `v` instead, which reruns the suite
too.)

## License note (for OSS publication)

The Microchip **IEC 60730 Class B Diagnostic Library** ships full C
sources on npm (the `@mchp-mcc` scope is MCC Melody's content-delivery
channel; Microchip's *supported* route is their gated Functional Safety
package, which carries different version numbers), but under Microchip's
"use with Microchip products" terms — not OSI. It is therefore **not
vendored** here;
`host/fetch_classb_ref.sh` downloads it into `ref/` (gitignore it) for
study. The tests in `src/` are original implementations of
public-literature algorithms (March C-: van de Goor; checkerboard register
walk; CRC-16/CCITT) and carry this repo's license. The library targets
tinyAVR 0/1/2 and AVR-DA/DB only — classic parts like the t461a were never
supported, which is why this suite exists.
