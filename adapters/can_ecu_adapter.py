#!/usr/bin/env python3
"""Translate selected CAN-bus-simulator messages into CARLA DBC frames.

The adapter is intentionally explicit and one-way:

    CAN-bus-simulator (vcan1) -> adapter -> CARLA network (vcan0)

It does not forward unknown frames and does not infer message ownership from
traffic. The checked-in ownership registry is the source of truth for the
translation boundary.
"""

import argparse
import json
import logging
import time
from pathlib import Path
from typing import Any, Dict, Optional

import can
import cantools

from can_network.dbc import LIGHT_SIGNALS, REQUIRED_SIGNALS


LOGGER = logging.getLogger("can_ecu_adapter")


def load_ownership(path: Path) -> Dict[int, Dict[str, Any]]:
    """Load the ownership registry and index entries by numeric CAN ID."""
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_version") != 1:
        raise ValueError(f"Unsupported ownership schema in {path}")

    entries = {}
    for item in payload.get("messages", []):
        can_id = int(item["can_id"], 0)
        if can_id in entries:
            raise ValueError(f"Duplicate CAN ID in ownership registry: {item['can_id']}")
        entries[can_id] = item
    return entries


class CarlaCanAdapter:
    """One-way translator from simulator frames to CARLA DBC frames."""

    def __init__(self, database: Any, ownership: Dict[int, Dict[str, Any]], output_bus: Any):
        self.database = database
        self.ownership = ownership
        self.output_bus = output_bus
        self.lights = 0

    @staticmethod
    def _single_byte(message: can.Message) -> int:
        if not message.data:
            raise ValueError("expected at least one data byte")
        return int(message.data[0])

    def _build_single_signal(self, name: str, value: int) -> can.Message:
        dbc_message = self.database.get_message_by_name(name)
        signal_name = REQUIRED_SIGNALS[name]
        return can.Message(
            arbitration_id=dbc_message.frame_id,
            data=dbc_message.encode({signal_name: value}),
            is_extended_id=dbc_message.is_extended_frame,
        )

    def _build_lights(self, signal_value: int) -> can.Message:
        # The source simulator uses 0=off, 1=left, 2=right, 3=hazards.
        self.lights &= ~(0x010 | 0x020)
        if signal_value in (1, 3):
            self.lights |= LIGHT_SIGNALS["LIGHTS_LeftBlinker_signal"]
        if signal_value in (2, 3):
            self.lights |= LIGHT_SIGNALS["LIGHTS_RightBlinker_signal"]

        dbc_message = self.database.get_message_by_name("GENERAL_LIGHTS")
        signals = {
            signal_name: int(bool(self.lights & flag))
            for signal_name, flag in LIGHT_SIGNALS.items()
        }
        return can.Message(
            arbitration_id=dbc_message.frame_id,
            data=dbc_message.encode(signals),
            is_extended_id=dbc_message.is_extended_frame,
        )

    def translate(self, source: can.Message) -> Optional[can.Message]:
        entry = self.ownership.get(source.arbitration_id)
        if entry is None or not entry.get("carla_message"):
            return None

        transform = entry.get("transform")
        target_name = entry["carla_message"]
        value = self._single_byte(source)

        if transform == "binary_to_u8":
            return self._build_single_signal(target_name, 255 if value else 0)
        if transform == "turn_signal_to_lights":
            return self._build_lights(value)
        raise ValueError(f"Unknown transform '{transform}' for 0x{source.arbitration_id:X}")

    def handle(self, source: can.Message, dry_run: bool = False) -> bool:
        target = self.translate(source)
        if target is None:
            return False
        if dry_run:
            LOGGER.info(
                "DRY-RUN 0x%03X#%s -> 0x%03X#%s",
                source.arbitration_id,
                bytes(source.data).hex().upper(),
                target.arbitration_id,
                bytes(target.data).hex().upper(),
            )
        else:
            self.output_bus.send(target)
            LOGGER.info(
                "0x%03X#%s -> 0x%03X#%s",
                source.arbitration_id,
                bytes(source.data).hex().upper(),
                target.arbitration_id,
                bytes(target.data).hex().upper(),
            )
        return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Bridge selected simulated ECU signals into CARLA CAN frames.")
    parser.add_argument("--source", default="vcan1", help="Simulator CAN interface")
    parser.add_argument("--target", default="vcan0", help="CARLA CAN interface")
    parser.add_argument("--dbc", default="data/carla.dbc", help="CARLA DBC path")
    parser.add_argument(
        "--ownership",
        default="integration/can_ecu_ownership.json",
        help="Message ownership registry",
    )
    parser.add_argument("--duration", type=float, default=None, help="Stop after this many seconds")
    parser.add_argument("--max-frames", type=int, default=None, help="Stop after this many translated frames")
    parser.add_argument("--dry-run", action="store_true", help="Decode and log without transmitting")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.duration is not None and args.duration <= 0:
        raise SystemExit("duration must be positive")
    if args.max_frames is not None and args.max_frames <= 0:
        raise SystemExit("max-frames must be positive")

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    ownership = load_ownership(Path(args.ownership))
    database = cantools.database.load_file(args.dbc)
    source_bus = can.Bus(interface="socketcan", channel=args.source, receive_own_messages=False)
    target_bus = None if args.dry_run else can.Bus(
        interface="socketcan", channel=args.target, receive_own_messages=False
    )
    adapter = CarlaCanAdapter(database, ownership, target_bus)
    translated = 0
    started = time.monotonic()

    try:
        while True:
            if args.duration is not None and time.monotonic() - started >= args.duration:
                break
            if args.max_frames is not None and translated >= args.max_frames:
                break
            message = source_bus.recv(timeout=0.5)
            if message is None:
                continue
            try:
                if adapter.handle(message, dry_run=args.dry_run):
                    translated += 1
            except (KeyError, TypeError, ValueError) as exc:
                LOGGER.warning("Could not translate 0x%03X: %s", message.arbitration_id, exc)
    except KeyboardInterrupt:
        LOGGER.info("Adapter stopped")
    finally:
        source_bus.shutdown()
        if target_bus is not None:
            target_bus.shutdown()

    LOGGER.info("Translated %d frame(s)", translated)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
