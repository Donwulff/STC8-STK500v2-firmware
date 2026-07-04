#!/usr/bin/env python3
"""Passive pin fingerprint of whatever sits in the programmer's socket.

Runs a FIXED sequence so runs are diffable against each other and against
a known-good ("golden") part:

  1. port snapshot         (pindbg 'p')
  2. VCC + sag guard       (pindbg 'M')
  3. ADC float level of every mapped socket pin  ('<pin>' + 'A')
  4. decay probe           (pindbg 'C')

Socket is expected OFF (safe idle); the decay probe drives pins briefly
but never powers the socket.  Output goes to stdout — redirect/tee to
bank a run.  NOTE: float levels and decay times depend on the specimen's
FUSE state (an enabled crystal oscillator cell biases its XTAL pins) and
on how hard the die loads the phantom rail — compare like with like.

Usage:  fingerprint.py [--port /dev/ttyACM0] [--off]
"""

import argparse
import sys
import time

from run_selftest import open_port, send, read_avail, sh

# pindbg pin-select letters worth fingerprinting, in canonical order.
# (skips 'd' = red-LED dummy and 'v' = VCC_EN switch)
PINS = [
    ("f", "WR    hdr"),
    ("a", "XA0   hdr"),
    ("b", "XA1   hdr"),
    ("c", "BS1   hdr"),
    ("h", "XTAL1 hdr"),
    ("e", "OE    hdr"),
    ("g", "PAGEL hdr"),
    ("l", "ISP-RST  "),
    ("r", "RESET_DRV"),
    ("x", "P3.3  ref"),
    ("y", "P3.4  ref"),
    ("z", "P3.7  ref"),
]


def enter_pindbg(fd):
    """Return with pindbg active, without ever sending the entry token
    into an already-active pindbg (its 'B' would jump to bootloader)."""
    send(fd, "p", settle=0.4)
    if "P0=" in read_avail(fd):
        return
    send(fd, "q", settle=0.5)           # harmless noise in STK mode
    read_avail(fd)
    send(fd, "@PINDBG!", settle=0.5)
    read_avail(fd)
    send(fd, "p", settle=0.4)
    if "P0=" not in read_avail(fd):
        sys.exit("could not enter pindbg — is the board in STK mode?")


def drain_until(fd, token, tries=30):
    out = ""
    for _ in range(tries):
        chunk = read_avail(fd, timeout=0.5)
        out += chunk
        if token in out:
            break
        if not chunk:
            time.sleep(1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--off", action="store_true",
                    help="power the socket down (safe idle) when done")
    args = ap.parse_args()

    sh(f"fuser -k {args.port} 2>/dev/null; sleep 1")
    fd = open_port(args.port)
    enter_pindbg(fd)

    print(f"# fingerprint {time.strftime('%Y-%m-%d %H:%M:%S')} "
          f"port={args.port}")

    send(fd, "p", settle=0.4)
    print("## ports\n" + read_avail(fd).strip())

    send(fd, "M", settle=0.6)
    print("## vcc\n" + read_avail(fd).strip())

    print("## adc floats (mV, socket off = phantom-rail levels)")
    for letter, name in PINS:
        send(fd, letter, settle=0.2)
        read_avail(fd)
        send(fd, "A", settle=1.2)
        val = " ".join(read_avail(fd, timeout=0.5).split())
        print(f"  {name} : {val}")

    print("## decay probe")
    send(fd, "C", settle=2.0)
    print(drain_until(fd, "PROBE DONE").strip())

    if args.off:
        send(fd, "v", settle=0.2)
        send(fd, "+", settle=0.2)
        print("## socket powered down")
    print("# done (pindbg left active)")


if __name__ == "__main__":
    main()
