#!/usr/bin/env python3
"""Fail when the handwritten OMNeT++ topology drifts from the DBC manifest."""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "integration/omnet/ecu_manifest.json"
NED = ROOT / "omnet/ned/canlab/CanLabNetwork.ned"
INI = ROOT / "omnet/simulations/omnetpp.ini"


def main() -> int:
    errors: list[str] = []
    try:
        manifest = json.loads(MANIFEST.read_text())
        ned = NED.read_text()
        ini = INI.read_text()
        dbc_path = ROOT / manifest["dbc_file"]
        dbc_bytes = dbc_path.read_bytes()
    except (OSError, KeyError, json.JSONDecodeError) as exc:
        print(f"cannot read required config: {exc}", file=sys.stderr)
        return 2

    digest = hashlib.sha256(dbc_bytes).hexdigest()
    if digest != manifest.get("dbc_sha256"):
        errors.append("manifest DBC hash is stale; run tools/export_omnet_manifest.py")

    expected = {
        int(item["id"]): (item["producer"], int(item["cycle_ns"]))
        for item in manifest.get("messages", [])
    }
    actual: dict[int, tuple[str, int]] = {}
    block_re = re.compile(r'ecuName\s*=\s*"([^"]+)"\s*;\s*frameSpecs\s*=\s*"([^"]*)"', re.S)
    for producer, specs in block_re.findall(ned):
        for spec in specs.split(";"):
            if not spec:
                continue
            fields = spec.split(",")
            if len(fields) != 5:
                errors.append(f"NED frame spec for {producer} has {len(fields)} fields; expected 5")
                continue
            try:
                can_id = int(fields[0], 0)
                period_ns = int(fields[1])
            except ValueError:
                errors.append(f"NED has invalid ID or period in {spec!r}")
                continue
            if can_id in actual:
                errors.append(f"NED config declares CAN ID 0x{can_id:03x} more than once")
            actual[can_id] = (producer, period_ns)

    for can_id in sorted(expected.keys() - actual.keys()):
        item = next(msg for msg in manifest["messages"] if int(msg["id"]) == can_id)
        errors.append(f"NED omits 0x{can_id:03x} {item['name']} from {item['producer']}")
    for can_id in sorted(actual.keys() - expected.keys()):
        errors.append(f"NED declares unknown CAN ID 0x{can_id:03x}")
    for can_id in sorted(expected.keys() & actual.keys()):
        if actual[can_id] != expected[can_id]:
            errors.append(
                f"0x{can_id:03x} producer/period mismatch: NED={actual[can_id]}, "
                f"manifest={expected[can_id]}"
            )

    node_match = re.search(r'ecuNames\s*=\s*default\("([^"]+)"\)', NED.read_text())
    if not node_match:
        errors.append("CanBusModule ecuNames default is missing")
    else:
        ned_nodes = set(node_match.group(1).split(","))
        manifest_nodes = set(manifest.get("nodes", {}))
        if ned_nodes != manifest_nodes:
            errors.append(f"controller node mismatch: NED={sorted(ned_nodes)}, manifest={sorted(manifest_nodes)}")

    queue_match = re.search(r"int maxQueueDepth\s*=\s*default\((\d+)\)", ned)
    queue_capacity = manifest.get("bus", {}).get("queue_capacity")
    if not queue_match or queue_capacity != int(queue_match.group(1)):
        errors.append(f"bus queue capacity mismatch: NED={queue_match.group(1) if queue_match else None}, manifest={queue_capacity}")

    duration_match = re.search(r"\*\*\.bitDuration\s*=\s*([0-9.]+)(ns|us|ms|s)", ini)
    if not duration_match:
        errors.append("omnetpp.ini bitDuration is missing or uses an unsupported unit")
    else:
        value, unit = duration_match.groups()
        scale_ns = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}[unit]
        bit_ns = float(value) * scale_ns
        if bit_ns <= 0 or abs(1_000_000_000 / bit_ns - manifest.get("bitrate", 0)) > 1e-6:
            errors.append(f"bitrate mismatch: INI bitDuration={value}{unit}, manifest={manifest.get('bitrate')}")

    observer = re.search(r"simple PassiveObserver\s*\{(.*?)\n\}", ned, re.S)
    if not observer or re.search(r"\boutput\b", observer.group(1)):
        errors.append("passive observer must exist and have no output gate")
    if "bus.bitTap --> observer.observe;" not in ned:
        errors.append("the bus tap must connect to the passive observer")
    if re.search(r"-->\s*bus\.attackIn\b", ned):
        errors.append("the passive network must not connect a bus intervention input")
    for name in ("Baseline", "ConcurrentTraffic", "MissingInputControl", "PassiveErrorFixture"):
        if f"[Config {name}]" not in ini:
            errors.append(f"missing passive scenario {name}")

    if errors:
        print("OMNeT++ configuration drift detected:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"manifest matches DBC, {len(actual)} configured messages, node set, queue capacity, bitrate, and passive topology")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
