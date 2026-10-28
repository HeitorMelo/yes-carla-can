#!/usr/bin/env python3
"""Summarize and sanity-check OMNeT++ JSONL CAN event traces.

The checker verifies trace consistency (controller transitions, arbitration
counter behavior, and bus-off silence) and can enforce scenario assertions.
It does not infer controller state from application delivery results.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path


def load_events(path: Path) -> list[dict]:
    events = []
    with path.open() as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
            if not isinstance(event, dict) or "event" not in event:
                raise ValueError(f"{path}:{line_number}: expected object with event field")
            events.append(event)
    if not events:
        raise ValueError(f"{path}: trace contains no events")
    return events


def check_trace(events: list[dict]) -> list[str]:
    errors: list[str] = []
    bus_off_at: dict[tuple[str, str], int] = {}
    ordered = list(enumerate(events))
    previous_time: dict[str, int] = {}
    last_tec: dict[tuple[str, str], int] = {}
    open_attempts: dict[tuple[str, str, str, str], dict] = {}

    def attempt_key(item: dict) -> tuple[str, str, str, str]:
        return (str(item.get("run", "default")), str(item.get("ecu", "")),
                str(item.get("frame_uid", "")), str(item.get("attempt", "")))
    for index, event in ordered:
        event_name = event.get("event")
        ecu = event.get("ecu")
        run = str(event.get("run", "default"))
        time_ns = event.get("time_ns")
        if time_ns is not None and (not isinstance(time_ns, int) or time_ns < 0):
            errors.append(f"event {index}: time_ns must be a nonnegative integer")
        elif isinstance(time_ns, int):
            if run in previous_time and time_ns < previous_time[run]:
                errors.append(f"event {index}: time moves backward in run {run}")
            previous_time[run] = time_ns

        counters = {name: event.get(name) for name in
                    ("tec_before", "tec_after", "rec_before", "rec_after")}
        controller_events = {"bit_error", "receiver_error", "arbitration_lost", "tx_success", "bus_off"}
        if event_name in controller_events and not all(isinstance(value, int) for value in counters.values()):
            errors.append(f"event {index}: {event_name} requires a complete integer TEC/REC snapshot")
            continue
        if any(value is not None for value in counters.values()):
            if not all(isinstance(value, int) for value in counters.values()):
                errors.append(f"event {index}: controller counter snapshot is incomplete or non-integer")
                continue
            tb, ta = counters["tec_before"], counters["tec_after"]
            rb, ra = counters["rec_before"], counters["rec_after"]
            if not (0 <= tb <= 256 and 0 <= ta <= 256 and 0 <= rb <= 255 and 0 <= ra <= 255):
                errors.append(f"event {index}: counter snapshot is outside Classical CAN bounds")
            expected_state = "bus_off" if ta > 255 else "error_passive" if ta > 127 or ra > 127 else "error_active"
            if event.get("state") != expected_state:
                errors.append(f"event {index}: state {event.get('state')!r} conflicts with TEC/REC snapshot ({ta}/{ra})")
            if event_name == "bit_error" and ta != min(256, tb + 8):
                errors.append(f"event {index}: bit error TEC transition should be {tb} -> {min(256, tb + 8)}")
            if event_name == "receiver_error" and (ta != tb or ra != min(255, rb + 1)):
                errors.append(f"event {index}: receiver error must preserve TEC and increment REC by one")
            if event_name == "tx_success" and (ta != max(0, tb - 1) or ra != rb):
                errors.append(f"event {index}: transmit success must reduce TEC by one and preserve REC")
            if event_name == "bus_off" and (ta != 256 or tb > 255):
                errors.append(f"event {index}: bus-off requires TEC transition to 256")
            if event_name == "ack_error":
                passive_before = tb > 127 or rb > 127
                expected_tec = tb if passive_before else min(256, tb + 8)
                if ta != expected_tec:
                    errors.append(f"event {index}: ACK error TEC transition should be {tb} -> {expected_tec}")

            tracked_tec_events = {"tx_queue_admit", "tx_queue_reject", "tx_attempt", "tx_blocked_bus_off",
                                  "bit_error", "ack_error", "tx_success", "arbitration_lost",
                                  "receiver_error", "bus_off"}
            key = (run, str(ecu))
            if ecu and event_name in tracked_tec_events:
                last = last_tec.get(key)
                if event_name == "bus_off":
                    # bus_off is a state marker emitted immediately after the
                    # bit_error row for the same TEC transition.
                    if last is not None and last != ta:
                        errors.append(f"event {index}: bus-off marker does not follow the TEC 256 transition")
                else:
                    if last is None and tb != 0:
                        errors.append(f"event {index}: first observed TEC for {ecu} must start at zero")
                    elif last is not None and tb != last:
                        errors.append(f"event {index}: discontinuous TEC for {ecu}: previous {last}, next before={tb}")
                    last_tec[key] = ta
        if event_name == "tx_attempt" and ecu:
            open_attempts[attempt_key(event)] = event
        if event_name == "arbitration_lost" and ecu:
            before, after = event.get("tec_before"), event.get("tec_after")
            if before is not None and after is not None and before != after:
                errors.append(f"event {index}: arbitration loss changed TEC for {ecu} ({before} -> {after})")
            if event.get("rec_before") is not None and event.get("rec_before") != event.get("rec_after"):
                errors.append(f"event {index}: arbitration loss changed REC for {ecu}")
            if event.get("field") != "Arbitration":
                errors.append(f"event {index}: arbitration loss must occur in the Arbitration field")
            live_attempts = [attempt for key, attempt in open_attempts.items() if key[0] == run]
            contender_ecus = {attempt.get("ecu") for attempt in live_attempts}
            contender_ids = [attempt.get("can_id") for attempt in live_attempts
                             if isinstance(attempt.get("can_id"), int)]
            if len(contender_ecus) < 2:
                errors.append(f"event {index}: arbitration loss lacks simultaneous attempts from multiple ECUs")
            else:
                if ecu not in contender_ecus or not contender_ids:
                    errors.append(f"event {index}: losing ECU is absent from the simultaneous contender set")
                elif isinstance(event.get("can_id"), int) and event["can_id"] <= min(contender_ids):
                    errors.append(f"event {index}: lower CAN ID cannot lose arbitration to a higher ID")
            open_attempts.pop(attempt_key(event), None)
        if event_name == "bus_off" and ecu:
            key = (run, ecu)
            bus_off_at.setdefault(key, index)
            prior_bit_error = any(
                item.get("run", "default") == run and item.get("ecu") == ecu
                and item.get("event") == "bit_error" and item.get("tec_after") == 256
                and isinstance(time_ns, int) and isinstance(item.get("time_ns"), int)
                and item["time_ns"] <= time_ns
                for item in events[:index + 1]
            )
            if not prior_bit_error:
                errors.append(f"event {index}: bus-off lacks a preceding controller bit-error transition to TEC 256")
        if ecu and event_name in {"tx_attempt", "tx_success"}:
            key = (run, ecu)
            if key in bus_off_at:
                errors.append(f"event {index}: {event_name} recorded for {ecu} after bus-off")
        if event_name in {"tx_success", "bit_error", "ack_error"} and ecu:
            open_attempts.pop(attempt_key(event), None)
    return errors


def compare_attack_to_baseline(baseline: list[dict], attack: list[dict], victim: str,
                               victim_can_id: int, unaffected: str) -> tuple[list[str], dict]:
    errors = check_trace(baseline) + check_trace(attack)
    baseline_off = any(e.get("event") == "bus_off" and e.get("ecu") == victim for e in baseline)
    if baseline_off:
        errors.append(f"baseline unexpectedly records bus-off for {victim}")
    attack_off = [e for e in attack if e.get("event") == "bus_off" and e.get("ecu") == victim]
    if not attack_off:
        errors.append(f"attack trace has no bus-off event for {victim}")
        off_time = None
    else:
        off_time = attack_off[0].get("time_ns")

    def successful(messages: list[dict], ecu: str, can_id: int) -> list[dict]:
        return [e for e in messages if e.get("event") == "tx_success" and
                e.get("ecu") == ecu and e.get("can_id") == can_id]

    baseline_victim = successful(baseline, victim, victim_can_id)
    attack_victim = successful(attack, victim, victim_can_id)
    if not baseline_victim:
        errors.append(f"baseline has no successful 0x{victim_can_id:03x} transmissions from {victim}")
    if len(attack_victim) >= len(baseline_victim):
        errors.append("attack did not reduce successful victim-frame transmissions versus baseline")
    baseline_deliveries = [e for e in baseline if e.get("event") == "actuator_update" and
                           e.get("ecu") == "ActuatorGateway" and e.get("can_id") == victim_can_id]
    attack_deliveries = [e for e in attack if e.get("event") == "actuator_update" and
                         e.get("ecu") == "ActuatorGateway" and e.get("can_id") == victim_can_id]
    if not baseline_deliveries:
        errors.append(f"baseline has no delivered BRAKE command at ActuatorGateway")
    if len(attack_deliveries) >= len(baseline_deliveries):
        errors.append("attack did not reduce BRAKE deliveries to ActuatorGateway versus baseline")
    if isinstance(off_time, int) and any(e.get("time_ns", -1) >= off_time for e in attack_deliveries):
        errors.append("attack trace contains an ActuatorGateway BRAKE delivery at or after victim bus-off")

    targeted_errors = [e for e in attack if e.get("event") == "bit_error" and e.get("ecu") == victim
                       and e.get("can_id") == victim_can_id and e.get("field") == "Data"]
    drives = [e for e in attack if e.get("event") == "attack_drive" and e.get("can_id") == victim_can_id
              and e.get("field") == "Data"]
    target_attempts = [e for e in attack if e.get("event") == "tx_attempt" and e.get("ecu") == victim
                       and e.get("can_id") == victim_can_id]
    if not target_attempts:
        errors.append(f"attack trace contains no transmit attempt for {victim} 0x{victim_can_id:03x}")
    if not drives:
        errors.append(f"attack trace contains no data-bit attack drive for 0x{victim_can_id:03x}")
    if not targeted_errors:
        errors.append(f"attack trace contains no controller-detected data bit error for {victim} 0x{victim_can_id:03x}")
    elif drives:
        matching_attempt = any(
            (drive.get("frame_uid"), drive.get("attempt")) ==
            (attempt.get("frame_uid"), attempt.get("attempt"))
            for drive in drives for attempt in target_attempts
        )
        matching_fault = any(
            (drive.get("frame_uid"), drive.get("attempt"), drive.get("bit_index")) ==
            (fault.get("frame_uid"), fault.get("attempt"), fault.get("bit_index"))
            for drive in drives for fault in targeted_errors
        )
        if not matching_attempt or not matching_fault:
            errors.append("attack drive does not match a transmitted target bit and controller-detected error")

    unaffected_after_off = []
    if isinstance(off_time, int):
        unaffected_after_off = [e for e in attack if e.get("event") == "tx_success" and
                                e.get("ecu") == unaffected and isinstance(e.get("time_ns"), int) and
                                e["time_ns"] > off_time]
        if not unaffected_after_off:
            errors.append(f"unaffected ECU {unaffected} has no successful traffic after victim bus-off")
    metrics = {
        "victim": victim,
        "victim_can_id": f"0x{victim_can_id:03x}",
        "baseline_victim_successes": len(baseline_victim),
        "attack_victim_successes": len(attack_victim),
        "baseline_brake_deliveries": len(baseline_deliveries),
        "attack_brake_deliveries": len(attack_deliveries),
        "attack_bus_off_time_ns": off_time,
        "targeted_data_bit_errors": len(targeted_errors),
        "attack_drives": len(drives),
        "target_attempts": len(target_attempts),
        "unaffected_ecu": unaffected,
        "unaffected_successes_after_bus_off": len(unaffected_after_off),
    }
    return errors, metrics


def check_attack_stops_early(events: list[dict], victim: str, victim_can_id: int,
                             unaffected: str) -> tuple[list[str], dict]:
    errors = check_trace(events)
    drives = [e for e in events if e.get("event") == "attack_drive" and
              e.get("can_id") == victim_can_id and e.get("field") == "Data"]
    faults = [e for e in events if e.get("event") == "bit_error" and e.get("ecu") == victim and
              e.get("can_id") == victim_can_id and e.get("field") == "Data"]
    if len(drives) != 1:
        errors.append(f"early-stop run requires exactly one target attack drive; found {len(drives)}")
    if len(faults) != 1:
        errors.append(f"early-stop run requires exactly one target controller error; found {len(faults)}")
    if any(e.get("event") == "bus_off" and e.get("ecu") == victim for e in events):
        errors.append(f"early-stop run unexpectedly placed {victim} bus-off")
    if drives and faults and (drives[0].get("frame_uid"), drives[0].get("attempt"), drives[0].get("bit_index")) != \
            (faults[0].get("frame_uid"), faults[0].get("attempt"), faults[0].get("bit_index")):
        errors.append("early-stop attack drive does not match the detected bit error")
    fault_time = faults[0].get("time_ns") if faults else None
    recovered = [e for e in events if e.get("event") == "tx_success" and e.get("ecu") == victim and
                 e.get("can_id") == victim_can_id and isinstance(fault_time, int) and
                 isinstance(e.get("time_ns"), int) and e["time_ns"] > fault_time]
    if not recovered:
        errors.append(f"{victim} has no successful target frame after the one-shot fault")
    delivered_after_fault = [e for e in events if e.get("event") == "actuator_update" and
                             e.get("ecu") == "ActuatorGateway" and e.get("can_id") == victim_can_id and
                             isinstance(fault_time, int) and isinstance(e.get("time_ns"), int) and
                             e["time_ns"] > fault_time]
    if not delivered_after_fault:
        errors.append("ActuatorGateway receives no successful BRAKE command after the one-shot fault")
    unaffected_successes = [e for e in events if e.get("event") == "tx_success" and
                            e.get("ecu") == unaffected]
    if not unaffected_successes:
        errors.append(f"unaffected ECU {unaffected} has no successful traffic in the early-stop run")
    metrics = {
        "victim": victim,
        "victim_can_id": f"0x{victim_can_id:03x}",
        "target_attack_drives": len(drives),
        "target_bit_errors": len(faults),
        "target_successes_after_fault": len(recovered),
        "target_deliveries_after_fault": len(delivered_after_fault),
        "victim_bus_off": any(e.get("event") == "bus_off" and e.get("ecu") == victim for e in events),
        "unaffected_ecu": unaffected,
        "unaffected_successes": len(unaffected_successes),
    }
    return errors, metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="JSONL trace exported by OMNeT++")
    parser.add_argument("--expect-bus-off", action="append", default=[], metavar="ECU")
    parser.add_argument("--expect-silence", action="append", default=[], metavar="ECU",
                        help="require a bus-off transition and no later tx_attempt/tx_success")
    parser.add_argument("--require-event", action="append", default=[], metavar="EVENT")
    parser.add_argument("--compare-baseline", type=Path, help="compare this attack trace with a baseline JSONL trace")
    parser.add_argument("--victim", default="Chassis_ECU")
    parser.add_argument("--victim-can-id", type=lambda value: int(value, 0), default=0x601)
    parser.add_argument("--unaffected-ecu", default="Powertrain_ECU")
    parser.add_argument("--report", type=Path, help="write a Markdown paired-run summary")
    parser.add_argument("--expect-attack-stops-early", action="store_true",
                        help="require one target bit fault, recovery, no bus-off, and unaffected traffic")
    args = parser.parse_args()
    try:
        events = load_events(args.trace)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2

    errors = check_trace(events)
    pair_metrics = None
    if args.expect_attack_stops_early:
        early_errors, pair_metrics = check_attack_stops_early(
            events, args.victim, args.victim_can_id, args.unaffected_ecu
        )
        errors.extend(early_errors)
    if args.compare_baseline:
        try:
            baseline = load_events(args.compare_baseline)
        except (OSError, ValueError) as exc:
            print(exc, file=sys.stderr)
            return 2
        pair_errors, pair_metrics = compare_attack_to_baseline(
            baseline, events, args.victim, args.victim_can_id, args.unaffected_ecu
        )
        errors.extend(pair_errors)
    counts = collections.Counter(event.get("event", "<missing>") for event in events)
    for ecu in args.expect_bus_off:
        if not any(e.get("event") == "bus_off" and e.get("ecu") == ecu for e in events):
            errors.append(f"expected bus_off event for {ecu}")
    for ecu in args.expect_silence:
        transitions = [i for i, e in enumerate(events) if e.get("event") == "bus_off" and e.get("ecu") == ecu]
        if not transitions:
            errors.append(f"expected bus_off event for {ecu}")
            continue
        off_time = events[transitions[0]].get("time_ns", -1)
        for i, event in enumerate(events):
            if event.get("ecu") == ecu and event.get("event") in {"tx_attempt", "tx_success"}:
                if event.get("time_ns", -1) >= off_time:
                    errors.append(f"event {i}: {ecu} transmitted at or after bus-off")
    for required in args.require_event:
        if not counts[required]:
            errors.append(f"expected at least one {required} event")

    print(f"events={len(events)} types={dict(sorted(counts.items()))}")
    if pair_metrics is not None:
        print(f"paired_metrics={json.dumps(pair_metrics, sort_keys=True)}")
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            lines = ["# Paired CAN experiment report", "", "| Metric | Value |", "|---|---:|"]
            for key, value in pair_metrics.items():
                lines.append(f"| {key.replace('_', ' ')} | {value} |")
            lines.extend(["", "Trace checks: " + ("failed" if errors else "passed") + "."])
            args.report.write_text("\n".join(lines) + "\n")
    if errors:
        print("trace validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("trace validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
