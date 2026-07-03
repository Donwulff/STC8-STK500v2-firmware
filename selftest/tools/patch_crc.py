#!/usr/bin/env python3
"""Pad the raw flash image to FLASHEND-1 and append the CRC-16/CCITT-FALSE
reference (big-endian) in the last two bytes, where flash_crc.c expects it.

Usage: patch_crc.py <in.bin> <out.bin> [flash_size]
"""
import sys

FLASH_SIZE = 4096   # ATtiny461A


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    size = int(sys.argv[3], 0) if len(sys.argv) > 3 else FLASH_SIZE
    body_len = size - 2

    data = open(sys.argv[1], "rb").read()
    if len(data) > body_len:
        sys.exit(f"image {len(data)} bytes: does not leave 2 bytes for the "
                 f"CRC in {size} bytes of flash")
    body = data + b"\xFF" * (body_len - len(data))
    crc = crc16_ccitt(body)
    with open(sys.argv[2], "wb") as f:
        f.write(body + bytes([crc >> 8, crc & 0xFF]))
    print(f"flash image: {len(data)} bytes used, CRC16 0x{crc:04X} @ "
          f"0x{body_len:04X}")


if __name__ == "__main__":
    main()
