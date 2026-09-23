"""Tests for passive CAN communication surveillance."""

import csv
import json
from pathlib import Path

import can

from communication_surveillance import message_to_record, summarize_records, write_outputs


def test_message_to_record_preserves_frame():
    message = can.Message(arbitration_id=0x100, data=[0x7F], is_extended_id=False, timestamp=12.5)
    record = message_to_record(message)
    assert record["timestamp"] == 12.5
    assert record["can_id"] == "0x100"
    assert record["dlc"] == 1
    assert record["data"] == "7F"
    assert record["is_error_frame"] is False


def test_summary_groups_frames_by_arbitration_id():
    records = [
        {"timestamp": 1.0, "can_id": "0x100", "is_extended_id": False, "is_error_frame": False},
        {"timestamp": 1.1, "can_id": "0x100", "is_extended_id": False, "is_error_frame": False},
        {"timestamp": 2.0, "can_id": "0x200", "is_extended_id": True, "is_error_frame": True},
    ]
    summary = summarize_records(records)
    assert summary["frame_count"] == 3
    assert summary["unique_can_ids"] == 2
    assert summary["error_frame_count"] == 1
    assert summary["by_can_id"]["0x100"]["count"] == 2
    assert summary["by_can_id"]["0x100"]["mean_interarrival_seconds"] == 0.1


def test_write_outputs_creates_csv_and_json(tmp_path: Path):
    records = [{
        "timestamp": 1.0, "channel": "vcan0", "can_id": "0x100",
        "is_extended_id": False, "is_remote_frame": False, "is_error_frame": False,
        "dlc": 1, "data": "7F", "message_name": "THROTTLE",
        "decoded_signals": {"THROTTLE_signal": 127},
    }]
    output_csv = tmp_path / "surveillance.csv"
    output_json = tmp_path / "surveillance-statistics.json"
    write_outputs(records, output_csv, output_json)
    with output_csv.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    assert rows[0]["can_id"] == "0x100"
    assert json.loads(rows[0]["decoded_signals"])["THROTTLE_signal"] == 127
    assert json.loads(output_json.read_text(encoding="utf-8"))["frame_count"] == 1
