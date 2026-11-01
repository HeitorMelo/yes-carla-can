#!/usr/bin/env python3
"""Check passive Phase 1 arbitration, queue, and observation controls."""

import argparse
import json
from pathlib import Path


def load_events(path: Path) -> list[dict]:
    """Read non-empty JSON Lines records from a trace."""
    with path.open() as stream:
        return [json.loads(line) for line in stream if line.strip()]


def events(trace: list[dict], name: str) -> list[dict]:
    return [row for row in trace if row["event"] == name]


def delivery_signature(trace: list[dict]) -> list[tuple]:
    """Return delivery fields that should survive release-order changes."""
    return [
        (row["time_ns"], row["frame_uid"], row["can_id"], row["wire_samples"])
        for row in events(trace, "tx_success")
    ]


def check_arbitration(base: list[dict], swapped: list[dict]) -> list[tuple]:
    admissions = events(base, "tx_queue_admit")
    swapped_admissions = events(swapped, "tx_queue_admit")
    if not admissions or not swapped_admissions:
        raise ValueError("release-order control is missing queue admissions")
    if admissions[0]["ecu"] == swapped_admissions[0]["ecu"]:
        raise ValueError("release-order control did not change the first admitted ECU")

    for trace in (base, swapped):
        initial_attempts = [
            row for row in events(trace, "tx_attempt") if row["time_ns"] == 1
        ]
        initial_ids = {row["can_id"] for row in initial_attempts}
        if not {0x600, 0x601}.issubset(initial_ids):
            raise ValueError("expected simultaneous 0x600 and 0x601 attempts")

        successes = events(trace, "tx_success")
        if not successes or successes[0]["can_id"] != 0x600:
            raise ValueError("the lower ID did not win the initial arbitration")

        losses = events(trace, "arbitration_lost")
        lost_without_penalty = any(
            row["can_id"] == 0x601 and row["tec_before"] == row["tec_after"]
            for row in losses
        )
        if not lost_without_penalty:
            raise ValueError("missing loss without TEC penalty for 0x601")

        retried = any(
            row["can_id"] == 0x601 and row["attempt"] > 1 for row in successes
        )
        if not retried:
            raise ValueError("arbitration loser did not retry successfully")

    base_signature = delivery_signature(base)
    if base_signature != delivery_signature(swapped):
        raise ValueError("delivery sequence changed when simultaneous release order was swapped")
    return base_signature


def check_queue_pressure(trace: list[dict]) -> int:
    rejected = events(trace, "tx_queue_reject")
    completed_ids = {row["frame_uid"] for row in events(trace, "tx_success")}
    if not rejected or not completed_ids:
        raise ValueError("queue pressure did not exercise both rejection and delivery")
    if any(row["frame_uid"] in completed_ids for row in rejected):
        raise ValueError("a rejected frame was reported as delivered")
    if any(row["queue_depth"] > 2 for row in trace):
        raise ValueError("queue trace exceeds configured capacity")
    return len(rejected)


def check_no_ack(trace: list[dict]) -> None:
    ack_errors = events(trace, "ack_error")
    retries = events(trace, "retry_scheduled")
    successes = events(trace, "tx_success")
    if not ack_errors or not retries or successes:
        raise ValueError("no-ACK control must show errors and retries without successful transmission")
    if ack_errors[0]["tec_before"] != 0 or ack_errors[0]["tec_after"] != 8:
        raise ValueError("initial no-ACK counter update is incorrect")
    if events(trace, "rx_accept") or events(trace, "actuator_update"):
        raise ValueError("no-ACK control falsely reported a delivery")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    output = parser.parse_args().output

    base = load_events(output / "baseline/events.jsonl")
    swapped = load_events(output / "order-swap/events.jsonl")
    pressure = load_events(output / "queue-pressure/events.jsonl")
    no_ack = load_events(output / "no-ack/events.jsonl")

    signature = check_arbitration(base, swapped)
    rejected_count = check_queue_pressure(pressure)
    check_no_ack(no_ack)

    print(
        f"arbitration order swap preserved {len(signature)} deliveries; "
        f"queue rejected {rejected_count} frames"
    )


if __name__ == "__main__":
    main()
