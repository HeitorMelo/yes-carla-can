#!/usr/bin/env python3
"""Export deterministic OMNeT++ message metadata from the experiment DBC.

This intentionally handles the small DBC subset used by this repository. It
fails on malformed or duplicate message declarations instead of guessing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DBC = ROOT / "data/carla_multi_ecu.dbc"
OUTPUT = ROOT / "integration/omnet/ecu_manifest.json"
MESSAGE_RE = re.compile(r"^BO_ (\d+) (\w+): (\d+) (\w+)$")
SIGNAL_RE = re.compile(r"^\s+SG_ (\w+)")
CYCLE_RE = re.compile(r'^BA_ "GenMsgCycleTime" BO_ (\d+) (\d+);$', re.M)

CONTROLLERS = {
    "Powertrain_ECU": {"queue_capacity": 32, "acknowledge": True},
    "Chassis_ECU": {"queue_capacity": 32, "acknowledge": True},
    "ADAS_ECU": {"queue_capacity": 64, "acknowledge": True},
    "Body_ECU": {"queue_capacity": 16, "acknowledge": True},
    "ActuatorGateway": {"queue_capacity": 64, "acknowledge": True},
}


def parse_dbc(path: Path) -> dict:
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    cycles = {int(i): int(ms) for i, ms in CYCLE_RE.findall(text)}
    messages: list[dict] = []
    current: dict | None = None
    seen_ids: set[int] = set()

    for line_number, line in enumerate(text.splitlines(), 1):
        match = MESSAGE_RE.match(line)
        if match:
            if current:
                messages.append(current)
            ident, name, dlc, producer = match.groups()
            ident, dlc = int(ident), int(dlc)
            if ident in seen_ids:
                raise ValueError(f"line {line_number}: duplicate CAN ID 0x{ident:X}")
            if ident > 0x7FF or dlc > 8:
                raise ValueError(f"line {line_number}: unsupported frame {name}")
            if producer not in CONTROLLERS:
                raise ValueError(f"line {line_number}: unknown producer {producer}")
            seen_ids.add(ident)
            current = {
                "id": ident,
                "name": name,
                "dlc": dlc,
                "producer": producer,
                "cycle_ns": cycles.get(ident, 0) * 1_000_000,
                "signals": [],
            }
            continue
        signal = SIGNAL_RE.match(line)
        if signal and current:
            current["signals"].append(signal.group(1))
    if current:
        messages.append(current)

    if not messages:
        raise ValueError(f"no BO_ messages found in {path}")
    messages.sort(key=lambda item: item["id"])
    if any(not message["signals"] for message in messages):
        empty = [message["name"] for message in messages if not message["signals"]]
        raise ValueError(f"messages without signals: {', '.join(empty)}")
    return {
        "schema_version": 1,
        "dbc_file": str(path.relative_to(ROOT)),
        "dbc_sha256": hashlib.sha256(raw).hexdigest(),
        "time_unit": "ns",
        "bitrate": 500_000,
        "frame_format": "classical_can_standard_11bit",
        "bus": {"queue_capacity": 256},
        "nodes": CONTROLLERS,
        "messages": messages,
        "receiver_filters": {"ActuatorGateway": "all_standard_data_frames"},
        "timeout_policy": {"ActuatorGateway": "hold_last"},
        "attack": {"enabled": False, "profile": "bit_driver_demo"},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dbc", type=Path, default=DBC)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true", help="fail if output is stale")
    args = parser.parse_args()
    try:
        rendered = json.dumps(parse_dbc(args.dbc), indent=2) + "\n"
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"manifest export failed: {exc}", file=sys.stderr)
        return 2
    if args.check:
        try:
            actual = args.output.read_text()
        except OSError as exc:
            print(f"manifest check failed: {exc}", file=sys.stderr)
            return 1
        if actual != rendered:
            print(f"{args.output} is stale; run {Path(__file__).name}", file=sys.stderr)
            return 1
        print(f"manifest is current: {args.output}")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
