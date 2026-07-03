# Repository Guidelines

## Project Structure & Module Organization

`firmware/` is the active project. Treat sibling directories such as `avrdude/`,
`ScratchMonkey/`, and `STC8H8K64U-DEMO-CODE-V9.6/` as reference material unless a
task explicitly says otherwise. `firmware/src/` contains the STK500v2 protocol,
HVPP/ISP engines, USB CDC, UART2, board HAL, and target entry point.
`firmware/host/` contains the gcc mock HAL, PTY harness, and host tests.
`firmware/test/run_gate.sh` is the pre-flash gate. `firmware/doc/` holds notes;
`firmware/build/` is generated output and should not be edited by hand.

## Build, Test, and Development Commands

Run these from `firmware/`:

- `make host` builds the host harness and unit-test binary with gcc.
- `make test` runs `build/test_remap`.
- `make gate` runs unit tests plus real `avrdude` sessions against simulated
  t461a, t2313, and m8 targets. This is required before flashing.
- `make fw` builds the SDCC target firmware at `build/fw.bin`.
- `make flash CONFIRM=yes` flashes with `stc8usb`; use only after a green gate
  and an explicit go/no-go decision.
- `make clean` removes generated build output.

## Coding Style & Naming Conventions

Use C99 and keep shared core code portable between SDCC and gcc. Follow the
existing style: 4-space indentation, return type on its own line for functions,
lower_snake_case for functions and variables, and uppercase names for macros,
register constants, and enum-like protocol constants. Prefer fixed-width integer
types for protocol bytes and hardware-facing values. Keep target-specific code
behind HAL interfaces. Comments should explain protocol or hardware behavior,
not restate simple assignments.

## Testing Guidelines

Add host-side tests in `firmware/host/` for protocol, remap, and HVPP behavior.
Use behavior-focused `test_*` function names and keep fixtures close to the test
that needs them. Run `make test` for focused checks and `make gate` before any
change that could affect transport, control-stack playback, signatures, fuses,
or flashing safety. Gate logs must show no simulator warnings and no writes in
read-only sessions.

## Commit & Pull Request Guidelines

No readable Git history is available in this workspace. Use short imperative
commit subjects, for example `Add x61 remap gate case`, with a body when hardware
risk, bench evidence, or protocol behavior needs context. Pull requests should
state scope, hardware impact, commands run, and related issue or plan links.
Include relevant `make gate` or `avrdude` excerpts for firmware and transport
changes.
