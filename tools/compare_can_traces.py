#!/usr/bin/env python3
"""Compare two event traces after excluding explicitly run-dependent fields."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def load(path: Path) -> tuple[bytes, list[dict]]:
    raw = path.read_bytes()
    rows = []
    for line_number, line in enumerate(raw.splitlines(), 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{path}:{line_number}: {exc}") from exc
        if not isinstance(item, dict) or "event" not in item:
            raise ValueError(f"{path}:{line_number}: expected event object")
        rows.append(item)
    if not rows:
        raise ValueError(f"{path}: empty trace")
    return raw, rows


def normalized(rows: list[dict], excluded: set[str]) -> tuple[str, list[str]]:
    lines = []
    for row in rows:
        view = {key: value for key, value in row.items() if key not in excluded}
        lines.append(json.dumps(view, sort_keys=True, separators=(",", ":")))
    text = "\n".join(lines) + "\n"
    return hashlib.sha256(text.encode()).hexdigest(), lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    parser.add_argument("--exclude", action="append", default=["run_number"],
                        help="field excluded from event comparison (default: run_number)")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--require-identical", action="store_true")
    args = parser.parse_args()
    try:
        raw_a, rows_a = load(args.first)
        raw_b, rows_b = load(args.second)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2
    hash_a, norm_a = normalized(rows_a, set(args.exclude))
    hash_b, norm_b = normalized(rows_b, set(args.exclude))
    identical = norm_a == norm_b
    raw_identical = raw_a == raw_b
    first_difference = next((i for i, (a, b) in enumerate(zip(norm_a, norm_b)) if a != b), None)
    if first_difference is None and len(norm_a) != len(norm_b):
        first_difference = min(len(norm_a), len(norm_b))
    result = {
        "first_trace": str(args.first),
        "second_trace": str(args.second),
        "events_first": len(rows_a),
        "events_second": len(rows_b),
        "raw_sha256_first": hashlib.sha256(raw_a).hexdigest(),
        "raw_sha256_second": hashlib.sha256(raw_b).hexdigest(),
        "normalized_sha256_first": hash_a,
        "normalized_sha256_second": hash_b,
        "excluded_fields": sorted(set(args.exclude)),
        "bytewise_identical": raw_identical,
        "identical_after_exclusions": identical,
        "first_different_event_index": first_difference,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text("# Trace repeatability report\n\n" +
                               "```json\n" + json.dumps(result, indent=2, sort_keys=True) + "\n```\n")
    if args.require_identical and not identical:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
