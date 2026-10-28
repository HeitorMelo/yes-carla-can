#!/usr/bin/env python3
"""Validate passive observations against independent CAN reference rules."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

from validate_can_vectors import bits, reference


def rows(path: Path) -> list[dict]:
    with path.open() as stream:
        return [json.loads(line) for line in stream if line.strip()]


def expected_wire(can_id: int, payload: bytes) -> list[tuple[bool, str, int, bool]]:
    """Build a sample/field reference independently of the C++ codec."""
    source: list[tuple[int, str, int]] = [(0, "SOF", 0)]
    source += [(v, "Arbitration", i + 1) for i, v in enumerate(bits(can_id, 11))]
    source += [(0, "Arbitration", 12), (0, "Control", 13), (0, "Control", 14)]
    source += [(v, "Control", i + 15) for i, v in enumerate(bits(len(payload), 4))]
    for i, v in enumerate([b for byte in payload for b in bits(byte, 8)]):
        source.append((v, "Data", i + 19))
    crc, _, _ = reference(can_id, payload)
    source += [(v, "CRC", i) for i, v in enumerate(bits(crc, 15))]
    result: list[tuple[bool, str, int, bool]] = []
    previous = None
    run = 0
    for value, field, index in source:
        result.append((not bool(value), field, index, False))
        run = run + 1 if value == previous else 1
        previous = value
        if run == 5:
            value = 1 - value
            result.append((not bool(value), field, -1, True))
            previous, run = value, 1
    result.extend([(False, "CRC_Delimiter", 0, False), (True, "ACK", 0, False),
                   (False, "ACK_Delimiter", 0, False)])
    result.extend((False, "EOF", i, False) for i in range(7))
    result.extend((False, "Intermission", i, False) for i in range(3))
    return result


def validate(directory: Path, freshness_mode: str) -> None:
    events = rows(directory / "events.jsonl")
    observations = rows(directory / "wire-observations.jsonl")
    freshness = rows(directory / "freshness.jsonl")
    successes = [event for event in events if event["event"] == "tx_success"]
    wire_frames = [event for event in events if event["event"] == "frame_wire"]
    groups: dict[int, list[dict]] = defaultdict(list)
    for item in observations:
        groups[item["frame"]].append(item)
    if not successes or len(groups) != len(successes) or len(wire_frames) != len(successes):
        raise ValueError(f"expected one observation sequence per completed frame: {len(groups)} / {len(successes)}")
    for frame_no, success in enumerate(successes):
        group = groups[frame_no]
        expected = expected_wire(success["can_id"], bytes.fromhex(success["payload_hex"]))
        if len(group) != len(expected):
            raise ValueError(f"frame {frame_no}: expected {len(expected)} bits, got {len(group)}")
        samples = "".join("1" if item["dominant"] else "0" for item in group)
        if samples != wire_frames[frame_no]["wire_samples"] or not samples.startswith(success["wire_samples"]):
            raise ValueError(f"frame {frame_no}: observer samples differ from bus samples")
        for index, (item, expected_bit) in enumerate(zip(group, expected)):
            actual = (item["dominant"], item["field"], item["logical_index"], item["stuffed"])
            if actual != expected_bit or not item["valid"] or item["wire_index"] != index:
                raise ValueError(f"frame {frame_no}, bit {index}: {actual} != {expected_bit}")
            if item["interval_ns"] != (0 if index == 0 else 2000):
                raise ValueError(f"frame {frame_no}, bit {index}: unexpected sample interval")
            if item["can_id"] not in (-1, success["can_id"]):
                raise ValueError(f"frame {frame_no}, bit {index}: wrong decoded identifier")
        if group[-1]["can_id"] != success["can_id"]:
            raise ValueError(f"frame {frame_no}: identifier was not decoded")
    states = [item["state"] for item in freshness]
    if freshness_mode == "missing":
        if states != ["fresh", "stale"] or freshness[-1]["age_ns"] != 150_000_000:
            raise ValueError(f"missing-input freshness control failed: {freshness}")
    elif freshness_mode == "healthy" and (not states or any(state != "fresh" for state in states)):
        raise ValueError("healthy input unexpectedly stale or no brake delivery")
    print(f"validated {len(successes)} frames, {len(observations)} passive samples, and freshness mode {freshness_mode}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--freshness", choices=("healthy", "missing", "unchecked"), default="healthy")
    args = parser.parse_args()
    validate(args.directory, args.freshness)


if __name__ == "__main__":
    main()
