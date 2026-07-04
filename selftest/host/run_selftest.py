#!/usr/bin/env python3
"""Flash the t461a self-test through the STC8 programmer board and decode
the results it strobes back over the HVPP socket wiring.  stdlib only.

Flow (see README.md):
  1. kill stray port readers, flash via avrdude (board's STK500v2 ISP path)
  2. enter pindbg ('@PINDBG!'), power the socket ('v' + '-', active low)
  3. poll 'p': PA result bus arrives on P2, strobe on P1 bit3 — collect
     one full 9-frame round between 0xA5 sync markers and decode it
  4. optionally measure the clock beacon: 'h' + 'W' ADC capture on ch8

Usage:
  run_selftest.py --port /dev/ttyACM0 --hex build/selftest.hex
  run_selftest.py --no-flash --port /dev/ttyACM0        # chip already running
  add --beacon for the RC-clock measurement, --off to power down after
"""
import argparse
import os
import re
import select
import subprocess
import sys
import termios
import time

FRAME_NAMES = ["sync", "CPU registers", "SRAM March C-", "flash CRC",
               "EEPROM", "ADC/VCC window", "watchdog", "VCC mV/32", "MCUSR"]

STATUS = {0xA: "PASS", 0xF: "FAIL", 0x0: "off/skipped", 0xE: "UNEXPECTED"}


def sh(cmd, timeout=120):
    print(f"+ {cmd}")
    try:
        return subprocess.call(cmd, shell=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"TIMEOUT after {timeout}s: {cmd}")
        return 1


def read_fuse(port, prog, fuse):
    """Read one fuse byte via avrdude; returns int or None."""
    cmd = f"avrdude -c {prog} -P {port} -p t461a -qq -U {fuse}:r:-:h"
    print(f"+ {cmd}")
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                           timeout=60)
    except subprocess.TimeoutExpired:
        print("TIMEOUT after 60s (is pindbg still active on the port?)")
        return None
    for tok in reversed((r.stdout + r.stderr).split()):
        try:
            return int(tok, 16)
        except ValueError:
            continue
    return None


def write_fuse(port, prog, fuse, val):
    return sh(f"avrdude -c {prog} -P {port} -p t461a "
              f"-U {fuse}:w:0x{val:02X}:m")


FUSES = ("lfuse", "hfuse", "efuse", "lock")


def read_all_fuses(port, prog):
    vals = {}
    for f in FUSES:
        v = read_fuse(port, prog, f)
        if v is None:
            return None, f
        vals[f] = v
    return vals, None


def fmt_fuses(vals):
    return " ".join(f"{k}=0x{v:02X}" for k, v in vals.items())


def lock_fixed_bits_ok(vals):
    """t461a lock byte bits 7:2 are hardwired 1.  Anything else means the
    'lock' read actually came from a different byte — the classic
    BS2-class wiring/mux aliasing telltale."""
    return (vals["lock"] & 0xFC) == 0xFC


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0            # iflag, oflag, lflag: raw
    attrs[2] = termios.CREAD | termios.CLOCAL | termios.CS8
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def send(fd, s, settle=0.15):
    os.write(fd, s.encode())
    time.sleep(settle)


def read_avail(fd, timeout=0.1):
    out = b""
    while True:
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return out.decode(errors="replace")
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            return out.decode(errors="replace")
        out += chunk
        timeout = 0.05


PORTS_RE = re.compile(
    r"P0=([0-9A-F]{2}) P1=([0-9A-F]{2}) P2=([0-9A-F]{2})")


def poll_ports(fd):
    """Send 'p', return (p0, p1, p2) or None."""
    send(fd, "p", settle=0.02)
    m = None
    for m in PORTS_RE.finditer(read_avail(fd, timeout=0.3)):
        pass                                       # keep the freshest line
    return tuple(int(m.group(i), 16) for i in (1, 2, 3)) if m else None


# pindbg selectors of every socket-wired pin (skip d=LED, r=12V!, l=ISP hdr)
WIRED_PINS = "01234567abcefgh"


def power_up(fd):
    """True power-on reset.  The STC's quasi-high pins backfeed the socket
    rail through the chip's clamp diodes, so under RSTDISBL (POR = the
    only reset) a plain VCC toggle can leave a browned-out boot stuck
    forever.  Kill the backfeed: drive every socket-wired pin low, let the
    rail really die, power on, release."""
    send(fd, "v"); send(fd, "+")
    for c in WIRED_PINS:
        send(fd, c, settle=0.03)
        send(fd, "-", settle=0.03)
    time.sleep(2.5)
    read_avail(fd)                                  # drain 'ok' echoes
    send(fd, "v"); send(fd, "-", settle=0.05)
    for c in WIRED_PINS:
        send(fd, c, settle=0.02)
        send(fd, "+", settle=0.02)
    read_avail(fd)


def collect_frames(fd, want=9, timeout_s=90):
    """Strobe-framed capture: latch P2 on each rising edge of P1 bit 3 (WR header)."""
    frames, prev_strobe = [], None
    deadline = time.time() + timeout_s
    last_diag = time.time()
    while time.time() < deadline:
        got = poll_ports(fd)
        if not got:
            continue
        p0, p1, p2 = got
        # strobe = AVR PB0, which is the x61 HVPP "WR" signal pin ->
        # WR header -> STC P1.3 (datasheet 2588F Table 18-12)
        strobe = bool(p1 & 0x08)
        if strobe and prev_strobe is False:
            frames.append(p2)
            print(f"  frame {len(frames):2d}: 0x{p2:02X}")
            # a full round is `want` frames starting at a 0xA5 sync
            for i, v in enumerate(frames):
                if v == 0xA5 and len(frames) - i >= want:
                    return frames[i:i + want]
        prev_strobe = strobe
        if not frames and time.time() - last_diag > 5:
            last_diag = time.time()
            print(f"  ... no frames yet: P0=0x{p0:02X} P1=0x{p1:02X} "
                  f"P2=0x{p2:02X}"
                  + ("  <- bus idle: chip is NOT running (reset held, or "
                     "browned-out boot needing a true POR)"
                     if p2 == 0xFF else ""))
        time.sleep(0.09)
    return None


def decode(round_):
    print("\n=== self-test results ===")
    ok = True
    for i in range(1, 7):
        v = round_[i]
        stat = STATUS.get(v & 0x0F, f"?0x{v:02X}")
        if v == 0x60:
            stat = "pending/off"
        if (v & 0x0F) in (0xF, 0xE):
            ok = False
        print(f"  {FRAME_NAMES[i]:<15} 0x{v:02X}  {stat}")
    mv = round_[7] * 32
    print(f"  {'VCC estimate':<15} {mv} mV" + ("" if mv else " (ADC off)"))
    m = round_[8]
    bits = [n for b, n in ((3, "WDRF"), (2, "BORF"), (1, "EXTRF"), (0, "PORF"))
            if m & (1 << b)]
    print(f"  {'MCUSR @ test':<15} 0x{m:02X}  ({'|'.join(bits) or 'none'})")
    print("=== overall:", "PASS" if ok else "FAIL", "===")
    return ok


def strobe_fcpu(fd, seconds=10):
    """F_CPU from the report strobe: each half-period is 38 x 8 ms of
    _delay_ms at the assumed 1 MHz, so wall-clock half-period vs 304 ms
    nominal gives the true clock ratio.  Far more trustworthy than the
    beacon 'W' capture, whose STC-side sample tick is uncalibrated."""
    prev, edges = None, []
    t0 = time.time()
    while time.time() - t0 < seconds:
        got = poll_ports(fd)
        if got:
            s = bool(got[1] & 0x08)
            if prev is not None and s != prev:
                edges.append(time.time())
            prev = s
        time.sleep(0.02)
    if len(edges) < 6:
        print("strobe clock: too few transitions, skipping")
        return
    avg = (edges[-1] - edges[0]) / (len(edges) - 1)
    print(f"strobe clock: {len(edges) - 1} half-periods, avg "
          f"{avg * 1000:.0f} ms (304 nominal) -> F_CPU ~ "
          f"{0.304 / avg:.2f} MHz-equivalent")


def beacon_hz(fd):
    """'h' selects the XTAL1 wire (STC P0.0 = ADC ch8), 'W' captures
    256 x ~1 ms.  Beacon toggles every 8 ms -> expect ~62.5 Hz."""
    send(fd, "h")
    send(fd, "W")
    buf, deadline = "", time.time() + 5
    while "CAPTURE DONE" not in buf and time.time() < deadline:
        buf += read_avail(fd, timeout=0.4)
    vals = [int(t) for line in buf.splitlines()
            for t in line.split() if t.isdigit()]
    vals = [v for v in vals if v <= 6000][1:]      # drop glitches + sample 0
    if len(vals) < 64:
        print("beacon: capture too short, skipping")
        return
    mid = (max(vals) + min(vals)) // 2
    rises = sum(1 for a, b in zip(vals, vals[1:]) if a < mid <= b)
    hz = rises / (len(vals) * 0.001)
    print(f"beacon: {rises} rising edges / {len(vals)} samples -> ~{hz:.1f} Hz "
          f"if the tick were 1 ms — RELATIVE measure only: the STC sample "
          f"tick is uncalibrated (0.7-1.3 ms observed); use the strobe "
          f"clock line for F_CPU")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--hex", default="build/selftest.hex")
    ap.add_argument("--no-flash", action="store_true")
    ap.add_argument("--no-pause", action="store_true",
                    help="skip the move-the-RESET-jumper prompt")
    ap.add_argument("--rstdisbl", action="store_true",
                    help="after flashing: prove the HVPP recovery path, "
                         "then program ONLY the RSTDISBL bit so the chip "
                         "runs with the RESET wire in place; undo with "
                         "--restore-reset")
    ap.add_argument("--restore-reset", action="store_true",
                    help="clear ONLY the RSTDISBL bit (read-modify-write "
                         "via HVPP, works on an ISP-dead part) and exit")
    ap.add_argument("--factory-fuses", action="store_true",
                    help="write factory 0x62/0xDF/0xFF via HVPP and exit "
                         "(lock bits untouched)")
    ap.add_argument("--beacon", action="store_true")
    ap.add_argument("--off", action="store_true",
                    help="power the socket down and leave pindbg afterwards")
    args = ap.parse_args()

    sh(f"fuser -k {args.port} 2>/dev/null; sleep 1")

    # ALWAYS bounce out of a possibly-active pindbg first: it eats every
    # byte (by design), which hangs avrdude silently.  Harmless when the
    # firmware is already in STK mode.
    fd = open_port(args.port)
    send(fd, "q", settle=0.5)
    os.close(fd)

    if args.restore_reset:
        # HVPP fuse write reaches a RSTDISBL'd part (that's the point of
        # this programmer).  Read-modify-write: ONLY bit 7 changes, the
        # rest of hfuse is whatever it was.  Then prove ISP is back.
        hv, bad = read_all_fuses(args.port, "stk500pp")
        if hv is None:
            sys.exit(f"HVPP {bad} read failed")
        print(f"  HVPP: {fmt_fuses(hv)}")
        if not lock_fixed_bits_ok(hv):
            sys.exit(f"ABORT: lock=0x{hv['lock']:02X} breaks the fixed-1 "
                     "bits — aliased reads, not writing anything")
        cur = hv["hfuse"]
        if cur & 0x80:
            print("RSTDISBL already unprogrammed — nothing to do.")
        elif write_fuse(args.port, "stk500pp", "hfuse", cur | 0x80):
            sys.exit("HVPP fuse restore failed")
        sys.exit(sh(f"avrdude -c stk500v2 -P {args.port} -p t461a -n"))

    if args.factory_fuses:
        # Explicit "back to factory" — via HVPP so it works on any chip
        # state (RSTDISBL, DWEN...).  Lock bits need a chip erase, not
        # touched here.
        rc = 0
        for fuse, val in (("lfuse", 0x62), ("hfuse", 0xDF), ("efuse", 0xFF)):
            rc |= write_fuse(args.port, "stk500pp", fuse, val)
        sys.exit(rc)

    if not args.no_flash:
        if sh(f"avrdude -c stk500v2 -P {args.port} -p t461a "
              f"-U flash:w:{args.hex}:i"):
            sys.exit("avrdude failed")
        time.sleep(1)

    if args.rstdisbl:
        # SAFETY PRE-FLIGHT: prove the HVPP recovery path on THIS chip
        # BEFORE making it the only way back in.  "Read then write the
        # same value" is NOT enough — an aliased read (mux/wiring fault)
        # would make us write a wrong value to the real byte.  So:
        #  1. read everything via BOTH transports; they use disjoint pins
        #     and mechanisms, agreement excludes aliasing;
        #  2. lock-byte fixed-1 bits (7:2) as the independent telltale;
        #  3. prove the HVPP WRITE lands where aimed by toggling the
        #     benign SELFPRGEN bit (efuse bit 0) and verifying through
        #     the OTHER transport, then restoring.  Read-path health
        #     never implies write-path health: reads pulse OE, writes
        #     pulse WR through different byte-select lines.
        print("--- HVPP pre-flight (recovery path must prove itself) ---")
        isp, bad = read_all_fuses(args.port, "stk500v2")
        if isp is None:
            sys.exit(f"ABORT: ISP {bad} read failed — not touching fuses")
        hv, bad = read_all_fuses(args.port, "stk500pp")
        if hv is None:
            sys.exit(f"ABORT: HVPP {bad} read failed — not touching fuses")
        print(f"  ISP : {fmt_fuses(isp)}\n  HVPP: {fmt_fuses(hv)}")
        for name, vals in (("ISP", isp), ("HVPP", hv)):
            if not lock_fixed_bits_ok(vals):
                sys.exit(f"ABORT: {name} lock=0x{vals['lock']:02X} breaks "
                         "the fixed-1 bits 7:2 — aliased reads "
                         "(BS2-class wiring/mux fault)")
        # physics cross-check: we are READING via ISP right now, so SPIEN
        # must be programmed (hfuse bit5=0).  A read claiming otherwise
        # is aliased regardless of what else looks plausible.
        if isp["hfuse"] & 0x20:
            sys.exit(f"ABORT: hfuse=0x{isp['hfuse']:02X} claims SPIEN "
                     "unprogrammed while ISP is demonstrably working — "
                     "aliased read")
        if isp != hv:
            sys.exit("ABORT: ISP and HVPP disagree — transport/mux fault")
        if hv["lfuse"] == hv["efuse"] and hv["lock"] == hv["hfuse"]:
            sys.exit("ABORT: classic aliasing fingerprint "
                     "(lfuse==efuse, lock==hfuse)")
        ef = hv["efuse"]
        if write_fuse(args.port, "stk500pp", "efuse", ef ^ 0x01):
            sys.exit("ABORT: HVPP efuse toggle write failed")
        toggled = read_fuse(args.port, "stk500v2", "efuse")
        rc = write_fuse(args.port, "stk500pp", "efuse", ef)
        restored = read_fuse(args.port, "stk500v2", "efuse")
        if rc or restored != ef:
            sys.exit(f"ABORT: efuse restore FAILED — reads "
                     f"0x{restored if restored is not None else -1:02X}, "
                     f"expected 0x{ef:02X}.  FIX THIS FIRST.")
        if toggled != (ef ^ 0x01):
            sys.exit(f"ABORT: HVPP write landed wrong (ISP read efuse="
                     f"0x{toggled:02X}, expected 0x{ef ^ 0x01:02X}); "
                     "original value verified restored")
        print("--- pre-flight PASS: HVPP read AND write proven; "
              "programming RSTDISBL ---")
        # RESET becomes GPIO -> chip ignores the GND-held HVPP node and
        # runs on power-on reset, wire untouched.  Read-modify-write:
        # ONLY bit 7 changes.
        cur = hv["hfuse"]
        if not (cur & 0x80):
            print("RSTDISBL already programmed.")
        elif write_fuse(args.port, "stk500v2", "hfuse", cur & 0x7F):
            sys.exit("RSTDISBL fuse write failed")
        print("RSTDISBL programmed — ISP is now DEAD on this chip until "
              "--restore-reset (HVPP).")

    # The HVPP RESET node is a 0V/12V-only driver (NPN holds GND by
    # default) — a chip wired to it can NEVER run.  The chip-side end of
    # the RESET jumper must come off: unplugged (internal pull-up) or,
    # better, onto the ISP-header RST pin (P1.5) for pindbg 'l' control —
    # unless --rstdisbl made the pin a GPIO.
    if not args.no_pause and not args.rstdisbl:
        input("\n>>> Move the chip's RESET wire OFF the HVPP RESET header\n"
              ">>> (unplug it, or move it to the ISP-header RST pin),\n"
              ">>> then press Enter to power up and collect results... ")

    fd = open_port(args.port)
    send(fd, "@PINDBG!", settle=0.5)
    print(read_avail(fd).strip())
    print("hard power cycle (backfeed-kill for a true POR)...")
    power_up(fd)
    print("socket powered, waiting for the suite "
          "(EEPROM pass ~3 s + WDT cycle)...")

    round_ = collect_frames(fd)
    ok = False
    if round_ is None:
        print("TIMEOUT: no complete frame round — is the chip running? "
              "('p' should show P1 bit3 toggling every ~300 ms)")
    else:
        ok = decode(round_)
        strobe_fcpu(fd)
        if args.beacon:
            beacon_hz(fd)

    if args.off:
        send(fd, "v")
        send(fd, "+")                              # VCC off
    else:
        print("(socket left powered, pindbg still active — "
              "'q' via terminal to exit)")
    os.close(fd)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
