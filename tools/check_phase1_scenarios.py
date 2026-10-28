#!/usr/bin/env python3
"""Check passive Phase 1 arbitration, queue, and observation controls."""
import argparse
import json
from pathlib import Path


def load(path):
    with path.open() as stream:
        return [json.loads(line) for line in stream if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    base = load(args.output / "baseline/events.jsonl")
    swapped = load(args.output / "order-swap/events.jsonl")
    pressure = load(args.output / "queue-pressure/events.jsonl")
    no_ack = load(args.output / "no-ack/events.jsonl")

    def events(trace, name):
        return [row for row in trace if row["event"] == name]

    admissions = events(base, "tx_queue_admit")
    swapped_admissions = events(swapped, "tx_queue_admit")
    if not admissions or not swapped_admissions or admissions[0]["ecu"] == swapped_admissions[0]["ecu"]:
        raise ValueError("release-order control did not change the first admitted ECU")
    for trace in (base, swapped):
        initial = [row for row in events(trace, "tx_attempt") if row["time_ns"] == 1]
        if not {0x600, 0x601}.issubset({row["can_id"] for row in initial}):
            raise ValueError("expected simultaneous 0x600 and 0x601 attempts")
        success = events(trace, "tx_success")
        if not success or success[0]["can_id"] != 0x600:
            raise ValueError("the lower ID did not win the initial arbitration")
        if not any(row["can_id"] == 0x601 and row["tec_before"] == row["tec_after"]
                   for row in events(trace, "arbitration_lost")):
            raise ValueError("missing loss without TEC penalty for 0x601")
        if not any(row["can_id"] == 0x601 and row["attempt"] > 1 for row in success):
            raise ValueError("arbitration loser did not retry successfully")
    signature = lambda trace: [(x["time_ns"], x["frame_uid"], x["can_id"], x["wire_samples"])
                               for x in events(trace, "tx_success")]
    if signature(base) != signature(swapped):
        raise ValueError("delivery sequence changed when simultaneous release order was swapped")

    rejects = events(pressure, "tx_queue_reject")
    completed = {x["frame_uid"] for x in events(pressure, "tx_success")}
    if not rejects or not completed:
        raise ValueError("queue pressure did not exercise both rejection and delivery")
    if any(x["frame_uid"] in completed for x in rejects):
        raise ValueError("a rejected frame was reported as delivered")
    if any(x["queue_depth"] > 2 for x in pressure):
        raise ValueError("queue trace exceeds configured capacity")
    ack_errors = events(no_ack, "ack_error")
    if not ack_errors or not events(no_ack, "retry_scheduled") or events(no_ack, "tx_success"):
        raise ValueError("no-ACK control must show errors and retries without successful transmission")
    if ack_errors[0]["tec_before"] != 0 or ack_errors[0]["tec_after"] != 8:
        raise ValueError("initial no-ACK counter update is incorrect")
    if events(no_ack, "rx_accept") or events(no_ack, "actuator_update"):
        raise ValueError("no-ACK control falsely reported a delivery")
    print(f"arbitration order swap preserved {len(signature(base))} deliveries; queue rejected {len(rejects)} frames")


if __name__ == "__main__":
    main()
