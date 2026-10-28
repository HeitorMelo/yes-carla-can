#!/usr/bin/env python3
"""Check the checked-in Classical CAN reference vectors."""

from __future__ import annotations

import json
from pathlib import Path

VECTORS = Path(__file__).resolve().parents[1] / "omnet/tests/can_reference_vectors.json"


def bits(value: int, width: int) -> list[int]:
    return [(value >> shift) & 1 for shift in range(width - 1, -1, -1)]


def reference(can_id: int, payload: bytes) -> tuple[int, int, list[dict[str, int]]]:
    # Classical CAN 2.0A data frame through the CRC sequence. Fields and bytes
    # are serialized most significant bit first.
    logical = [0] + bits(can_id, 11) + [0, 0, 0] + bits(len(payload), 4)
    for byte in payload:
        logical.extend(bits(byte, 8))

    remainder = 0
    for bit in logical:
        feedback = ((remainder >> 14) & 1) ^ bit
        remainder = (remainder << 1) & 0x7FFF
        if feedback:
            remainder ^= 0x4599

    crc = remainder
    wire_source = logical + bits(crc, 15)
    stuff_bits: list[dict[str, int]] = []
    previous: int | None = None
    run_length = 0
    wire_length = 0
    for logical_index, bit in enumerate(wire_source):
        wire_length += 1
        if bit == previous:
            run_length += 1
        else:
            previous, run_length = bit, 1
        if run_length == 5:
            inserted = 1 - bit
            stuff_bits.append({"after_logical_bit": logical_index, "inserted_bit": inserted})
            wire_length += 1
            previous, run_length = inserted, 1
    return crc, wire_length, stuff_bits


def main() -> int:
    vectors = json.loads(VECTORS.read_text())
    failures = []
    for vector in vectors["vectors"]:
        payload = bytes.fromhex(vector["payload_hex"])
        crc, wire_length, stuff = reference(vector["can_id"], payload)
        expected = (int(vector["crc15"], 16), vector["stuffed_region_wire_bits"], vector["stuff_bits"])
        actual = (crc, wire_length, stuff)
        if actual != expected:
            failures.append(f"{vector['name']}: expected {expected}, got {actual}")
        if vector["pre_crc_logical_bits"] != 1 + 11 + 3 + 4 + 8 * len(payload):
            failures.append(f"{vector['name']}: incorrect pre-CRC bit count")
    if failures:
        print("\n".join(failures))
        return 1
    print(f"validated {len(vectors['vectors'])} independent reference vectors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
