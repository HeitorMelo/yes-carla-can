#!/usr/bin/env python3
"""Passive CAN communication surveillance for the virtual CAN environment.

This module is deliberately read-only: it opens a CAN channel, receives
frames, and writes an evidence log and aggregate statistics. It never calls
``send`` and does not modify frames on the bus.
"""

import argparse
import csv
import json
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional

import can


CSV_FIELDS = [
    "timestamp", "channel", "can_id", "is_extended_id", "is_remote_frame",
    "is_error_frame", "dlc", "data", "message_name", "decoded_signals",
]


def _message_name_and_signals(message: can.Message, database: Any) -> tuple[str, Dict[str, Any]]:
    """Decode a frame when a DBC database is available."""
    if database is None:
        return "", {}
    try:
        dbc_message = database.get_message_by_frame_id(message.arbitration_id)
        decoded = database.decode_message(message.arbitration_id, message.data)
        return dbc_message.name, decoded
    except (KeyError, ValueError, TypeError):
        return "", {}


def message_to_record(message: can.Message, database: Any = None) -> Dict[str, Any]:
    """Convert a python-can message into a stable, CSV/JSON-friendly record."""
    name, signals = _message_name_and_signals(message, database)
    return {
        "timestamp": float(message.timestamp),
        "channel": getattr(message, "channel", ""),
        "can_id": f"0x{message.arbitration_id:X}",
        "is_extended_id": bool(message.is_extended_id),
        "is_remote_frame": bool(message.is_remote_frame),
        "is_error_frame": bool(message.is_error_frame),
        "dlc": int(message.dlc),
        "data": bytes(message.data).hex(" ").upper(),
        "message_name": name,
        "decoded_signals": signals,
    }


def capture_messages(
    bus: Any,
    duration: Optional[float] = None,
    max_frames: Optional[int] = None,
    clock: Callable[[], float] = time.monotonic,
) -> List[Dict[str, Any]]:
    """Capture frames from an already-open bus without transmitting anything."""
    records: List[Dict[str, Any]] = []
    started = clock()
    while True:
        if duration is not None and clock() - started >= duration:
            break
        if max_frames is not None and len(records) >= max_frames:
            break
        timeout = 0.5
        if duration is not None:
            timeout = max(0.0, min(timeout, duration - (clock() - started)))
        message = bus.recv(timeout=timeout)
        if message is not None:
            records.append(message_to_record(message))
    return records


def summarize_records(records: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    """Return surveillance statistics grouped by arbitration ID."""
    records = list(records)
    by_id: Dict[str, List[float]] = defaultdict(list)
    extended = Counter()
    errors = 0
    for record in records:
        can_id = record["can_id"]
        by_id[can_id].append(float(record["timestamp"]))
        extended[str(bool(record["is_extended_id"]))] += 1
        errors += int(bool(record["is_error_frame"]))

    per_id = {}
    for can_id, timestamps in sorted(by_id.items()):
        intervals = [b - a for a, b in zip(timestamps, timestamps[1:])]
        per_id[can_id] = {
            "count": len(timestamps),
            "first_timestamp": timestamps[0],
            "last_timestamp": timestamps[-1],
            "mean_interarrival_seconds": sum(intervals) / len(intervals) if intervals else None,
            "min_interarrival_seconds": min(intervals) if intervals else None,
            "max_interarrival_seconds": max(intervals) if intervals else None,
        }
    return {
        "frame_count": len(records),
        "unique_can_ids": len(per_id),
        "error_frame_count": errors,
        "frame_format_counts": dict(extended),
        "by_can_id": per_id,
    }


def write_outputs(records: List[Dict[str, Any]], output_csv: Path, output_json: Path) -> None:
    """Write the raw surveillance log and aggregate statistics."""
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for record in records:
            row = dict(record)
            row["decoded_signals"] = json.dumps(row["decoded_signals"], sort_keys=True)
            writer.writerow(row)
    with output_json.open("w", encoding="utf-8") as file:
        json.dump(summarize_records(records), file, indent=2)
        file.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Passively monitor CAN traffic and write a structured surveillance log."
    )
    parser.add_argument("--interface", default="vcan0", help="CAN interface (default: vcan0)")
    parser.add_argument("--duration", type=float, default=30.0, help="Capture duration in seconds")
    parser.add_argument("--max-frames", type=int, default=None, help="Stop after this many frames")
    parser.add_argument("--dbc", default="data/carla.dbc", help="Optional DBC file for signal decoding")
    parser.add_argument("--output-dir", default="traffic_logs", help="Output directory")
    parser.add_argument("--prefix", default="surveillance", help="Output filename prefix")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.duration <= 0 or (args.max_frames is not None and args.max_frames <= 0):
        raise SystemExit("duration and max-frames must be positive")
    database = None
    if args.dbc:
        try:
            import cantools
            database = cantools.database.load_file(args.dbc)
        except (OSError, ValueError) as exc:
            print(f"Warning: DBC decoding disabled: {exc}")

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output_dir = Path(args.output_dir)
    output_csv = output_dir / f"{args.prefix}-{timestamp}.csv"
    output_json = output_dir / f"{args.prefix}-{timestamp}-statistics.json"

    bus = can.Bus(interface="socketcan", channel=args.interface, receive_own_messages=False)
    records = []
    try:
        started = time.monotonic()
        while time.monotonic() - started < args.duration:
            if args.max_frames is not None and len(records) >= args.max_frames:
                break
            remaining = args.duration - (time.monotonic() - started)
            message = bus.recv(timeout=max(0.0, min(0.5, remaining)))
            if message is not None:
                records.append(message_to_record(message, database))
    finally:
        bus.shutdown()

    write_outputs(records, output_csv, output_json)
    print(f"Captured {len(records)} frames from {args.interface}")
    print(f"Raw log: {output_csv}")
    print(f"Statistics: {output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
