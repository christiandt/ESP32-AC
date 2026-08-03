#!/usr/bin/env python3
"""Regenerate the golden vectors used by test/test_proto/.

The point of these is that the expected bytes come from the *reference*
implementations, not from the C++ port. If the port drifts, the test fails
rather than the mistake being baked into the fixture.

Two sources:

  * Broadlink / Electrolux — transcribed verbatim from
    python-broadlink's ``Device.send_packet`` / ``scan`` and from
    ``electrolux-ac-cli``'s ``Electrolux._send``. Transcribed rather than
    imported so this runs with no dependencies and no device on the network;
    each function cites the code it mirrors, and any edit to it should be a
    copy of the upstream edit.

  * Midea — imported from ``msmart`` when it is installed, so those vectors are
    genuinely generated rather than transcribed.

Usage:

    python3 tools/gen_vectors.py            # human-readable, C++-pasteable hex
    python3 tools/gen_vectors.py --json     # machine-readable
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
from typing import Any, Dict

# ---------------------------------------------------------------------------
# Electrolux inner frame — mirrors electrolux/cli.py::Electrolux._send
# ---------------------------------------------------------------------------


def elux_frame(command: int, data: bytes) -> bytes:
    packet = bytearray(0xD)
    packet[0x00:0x02] = command.to_bytes(2, "little")
    packet[0x02:0x06] = bytes.fromhex("a5a55a5a")
    packet[0x08] = 0x01 if len(data) <= 2 else 0x02
    packet[0x09] = 0x0B
    # Two bytes into a one-byte slice: bytearray slice assignment resizes, so
    # the header ends up 14 bytes long and the payload starts at 0x0E. The C++
    # port spells that out instead of relying on the resize.
    packet[0xA:0xB] = len(data).to_bytes(2, "little")
    packet.extend(data)
    d_checksum = sum(packet[0x08:], 0xC0AD) & 0xFFFF
    packet[0x06:0x08] = d_checksum.to_bytes(2, "little")
    return bytes(packet)


# ---------------------------------------------------------------------------
# Broadlink outer packet — mirrors broadlink/device.py::Device.send_packet
# ---------------------------------------------------------------------------


def outer_header(devtype: int, packet_type: int, count: int, mac: bytes, dev_id: int,
                 payload: bytes) -> bytes:
    packet = bytearray(0x38)
    packet[0x00:0x08] = bytes.fromhex("5aa5aa555aa5aa55")
    packet[0x24:0x26] = devtype.to_bytes(2, "little")
    packet[0x26:0x28] = packet_type.to_bytes(2, "little")
    packet[0x28:0x2A] = count.to_bytes(2, "little")
    packet[0x2A:0x30] = mac[::-1]
    packet[0x30:0x34] = dev_id.to_bytes(4, "little")
    p_checksum = sum(payload, 0xBEAF) & 0xFFFF
    packet[0x34:0x36] = p_checksum.to_bytes(2, "little")
    return bytes(packet)


def outer_final_checksum(header: bytes, payload: bytes) -> int:
    # Note: zero padding, not PKCS7 — broadlink appends `bytes(padding)`.
    padding = (16 - len(payload)) % 16
    full = bytearray(header) + payload + bytes(padding)
    return sum(full, 0xBEAF) & 0xFFFF


# ---------------------------------------------------------------------------
# Broadlink discovery — mirrors broadlink/device.py::scan
# ---------------------------------------------------------------------------


def discovery_packet(local_ip: str, src_port: int, datetime_block: bytes = bytes(12)) -> bytes:
    packet = bytearray(0x30)
    packet[0x08:0x14] = datetime_block
    packet[0x18:0x1C] = socket.inet_aton(local_ip)[::-1]
    packet[0x1C:0x1E] = src_port.to_bytes(2, "little")
    packet[0x26] = 6
    checksum = sum(packet, 0xBEAF) & 0xFFFF
    packet[0x20:0x22] = checksum.to_bytes(2, "little")
    return bytes(packet)


# ---------------------------------------------------------------------------
# Midea — generated from msmart if available
# ---------------------------------------------------------------------------


def midea_vectors() -> Dict[str, Any]:
    try:
        import msmart.crc8 as crc8
    except ImportError:
        print("note: msmart-ng not installed; skipping Midea vectors "
              "(pip install msmart-ng)", file=sys.stderr)
        return {}

    query = bytes([0x41, 0x81, 0x00, 0xFF, 0x03, 0xFF, 0x00, 0x02] + [0x00] * 13)
    mutated = bytearray(query)
    mutated[2] = 0x01
    return {
        "crc8_empty": crc8.calculate(b""),
        "crc8_00": crc8.calculate(b"\x00"),
        "crc8_01": crc8.calculate(b"\x01"),
        "crc8_ff": crc8.calculate(b"\xff"),
        "crc8_query": crc8.calculate(query),
        "crc8_query_mutated": crc8.calculate(bytes(mutated)),
    }


def build() -> Dict[str, Any]:
    mac = bytes.fromhex("aabbccddeeff")

    status = elux_frame(0x0E, b"{}")
    temp = elux_frame(0x17, b'{"temp":22}')
    mode = elux_frame(0x19, b'{"ac_mode":0}')

    header = outer_header(0x4F9B, 0x6A, 0x8001, mac, 0x12345678, status)

    vectors: Dict[str, Any] = {
        "elux_status_frame": status.hex(),
        "elux_temp_frame": temp.hex(),
        "elux_mode_frame": mode.hex(),
        "outer_header": header.hex(),
        "outer_final_checksum": outer_final_checksum(header, status),
        "discovery_packet": discovery_packet("192.168.1.5", 12345).hex(),
    }
    vectors.update(midea_vectors())
    return vectors


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--json", action="store_true", help="emit JSON instead of a readable table")
    args = p.parse_args()

    vectors = build()

    if args.json:
        print(json.dumps(vectors, indent=2))
        return 0

    width = max(len(k) for k in vectors)
    for name, value in vectors.items():
        shown = f"0x{value:04X}" if isinstance(value, int) else value
        print(f"{name:<{width}}  {shown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
