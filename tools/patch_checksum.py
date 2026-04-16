"""
patch_checksum.py - ADuCM355 page 0 CRC32 patcher

The ADuCM355 boot ROM verifies a CRC32 over page 0 (0x0000-0x07F7) before
jumping to ResetISR. The GCC startup file (startup_adi_gcc.c) reserves the
4-byte checksum word at 0x07F8 as a 0xFFFFFFFF placeholder; this script
computes the CRC over page 0 and patches it in place.

Page 0 layout (2 KB = 0x800 bytes):
  0x000-0x7F7   Data covered by CRC (everything before the checksum word)
  0x7F8-0x7FB   CRC32 checksum word  <- CHECKSUM_OFFSET (patched here)
  0x7FC-0x7FF   Page 0 tail word (0xFFFFFFFF per ARM asm reference)

CRC algorithm: standard CRC-32 (ISO 3309 / IEEE 802.3)
  Polynomial : 0xEDB88320 (reflected)
  Initial    : 0xFFFFFFFF
  Final XOR  : 0xFFFFFFFF

The .bin input must be produced with:
  objcopy -O binary --gap-fill 0xFF <elf> <bin>
so that the gap region 0x1A0-0x7F7 contains 0xFF (erased flash state),
matching what the ROM kernel reads from flash when computing its own CRC.

Usage:
  python patch_checksum.py <bin_path> [hex_path]

  <bin_path>  Required. Raw binary to patch in place.
  [hex_path]  Optional. Intel-HEX file to re-emit from the patched .bin
              (same base address as the .bin, i.e. 0x00000000).
"""

import struct
import sys
from pathlib import Path

PAGE0_SIZE       = 0x800   # 2 KB - full page 0
CHECKSUM_OFFSET  = 0x7F8   # byte offset of the 4-byte CRC32 word


def crc32_adi(data):
    """Standard CRC-32 (poly 0xEDB88320) - matches ADuCM355 ROM kernel."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF


def patch_bin(bin_path):
    data = bytearray(Path(bin_path).read_bytes())

    # Pad to at least PAGE0_SIZE with 0xFF if binary is shorter
    while len(data) < PAGE0_SIZE:
        data.append(0xFF)

    # Reset the checksum field to 0xFFFFFFFF before computing
    # (the CRC must be computed over all other bytes, checksum field excluded)
    data[CHECKSUM_OFFSET:CHECKSUM_OFFSET + 4] = b'\xFF\xFF\xFF\xFF'

    # Compute CRC32 over bytes 0x000-0x7F7
    checksum = crc32_adi(data[:CHECKSUM_OFFSET])

    # Patch checksum into binary at CHECKSUM_OFFSET (little-endian)
    struct.pack_into("<I", data, CHECKSUM_OFFSET, checksum)

    Path(bin_path).write_bytes(data)
    print(f"Computed CRC32 over 0x000-0x{CHECKSUM_OFFSET - 1:03X}: 0x{checksum:08X}")
    print(f"Patched 0x{checksum:08X} -> offset 0x{CHECKSUM_OFFSET:03X} in {bin_path}")
    return bytes(data), checksum


def rewrite_hex(hex_path, bin_data):
    """Re-emit an Intel-HEX file from the patched binary.

    The original hex is overwritten. Base address is 0x00000000 (page 0 at
    flash start). Uses 16-byte data records, standard record types 00/01/04.
    """
    lines = []
    upper = 0xFFFF  # force an initial ELA record

    def ihex_line(record):
        # record is bytes of: bytecount | addr_hi | addr_lo | type | data...
        checksum = (-sum(record)) & 0xFF
        return ":" + record.hex().upper() + f"{checksum:02X}"

    RECORD_SIZE = 16
    addr = 0
    total = len(bin_data)
    i = 0
    while i < total:
        # Emit Extended Linear Address if upper 16 bits changed
        new_upper = (addr >> 16) & 0xFFFF
        if new_upper != upper:
            upper = new_upper
            ela = bytes([0x02, 0x00, 0x00, 0x04,
                         (upper >> 8) & 0xFF, upper & 0xFF])
            lines.append(ihex_line(ela))

        chunk = bin_data[i:i + RECORD_SIZE]
        lo = addr & 0xFFFF
        rec = bytes([len(chunk), (lo >> 8) & 0xFF, lo & 0xFF, 0x00]) + chunk
        lines.append(ihex_line(rec))

        addr += len(chunk)
        i += len(chunk)

    # EOF record
    lines.append(ihex_line(bytes([0x00, 0x00, 0x00, 0x01])))

    Path(hex_path).write_text("\n".join(lines) + "\n")
    print(f"Rewrote {hex_path} ({total} bytes, {len(lines)} records)")


def main(argv):
    if len(argv) < 2:
        print("usage: patch_checksum.py <bin_path> [hex_path]", file=sys.stderr)
        return 2

    bin_path = argv[1]
    hex_path = argv[2] if len(argv) > 2 else None

    if not Path(bin_path).is_file():
        print(f"error: {bin_path} not found", file=sys.stderr)
        return 1

    patched_bin, _ = patch_bin(bin_path)

    if hex_path:
        rewrite_hex(hex_path, patched_bin)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
