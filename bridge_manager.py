#!/usr/bin/env python3
"""Deterministic CARLA ↔ ZeroMQ ↔ FiCo4OMNeT lockstep bridge."""

from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import carla
import cantools
import pygame
import zmq


DEFAULT_DELTA = 0.02
ECU_BY_MESSAGE = {
    "THROTTLE": "Powertrain_ECU",
    "BRAKE": "Chassis_ECU",
    "STEER": "Chassis_ECU",
    "REVERSE": "Chassis_ECU",
    "HAND_BRAKE": "Chassis_ECU",
    "MANUAL_TRANSMISSION": "Chassis_ECU",
    "GEAR": "Chassis_ECU",
    "DOORS": "Chassis_ECU",
    "GENERAL_LIGHTS": "Chassis_ECU",
}
CONTROL_IDS = {
    "THROTTLE": 1536,
    "BRAKE": 1537,
    "STEER": 1538,
    "REVERSE": 1539,
    "HAND_BRAKE": 1540,
    "MANUAL_TRANSMISSION": 1542,
    "GEAR": 1543,
}


class CarlaCanLockstepBridge:
    def __init__(
        self,
        dbc_path: Path,
        fixed_delta: float = DEFAULT_DELTA,
        throttle: float = 0.7,
        brake: float = 1.0,
        steer: float = 0.0,
        host: str = "127.0.0.1",
        port: int = 2000,
        timeout: float = 30.0,
        rendering: bool = False,
        controls_window: bool = False,
    ) -> None:
        if fixed_delta <= 0:
            raise ValueError("fixed_delta must be positive")
        self.delta_t = fixed_delta
        self.controls = {
            "throttle": min(1.0, max(0.0, throttle)),
            "brake": min(1.0, max(0.0, brake)),
            "steer": min(1.0, max(-1.0, steer)),
            "reverse": False,
            "hand_brake": False,
            "manual_gear_shift": False,
            "gear": 1,
        }
        self.commands = self.controls.copy()
        self.db = cantools.database.load_file(str(dbc_path))
        self.client = carla.Client(host, port)
        self.client.set_timeout(timeout)
        self.timeout = timeout
        self.rendering = rendering
        self.controls_window = controls_window
        self.control_screen = None
        self.control_font = None
        self._controls_window_open = True
        self.world = None
        self.original_settings = None
        self.vehicle = None
        self.owns_vehicle = False
        self.context = zmq.Context()
        self.pub = self.context.socket(zmq.PUB)
        self.sub = self.context.socket(zmq.SUB)
        self.sync = self.context.socket(zmq.REQ)
        self._clock_ready = False
        self._last_control_time = {can_id: None for can_id in CONTROL_IDS.values()}
        self._schedule = []
        for message in self.db.messages:
            if message.cycle_time and message.cycle_time > 0:
                self._schedule.append((message, message.cycle_time / 1000.0, 0.0))

    def setup_carla(self) -> None:
        self.world = self.client.get_world()
        self.original_settings = self.world.get_settings()
        settings = self.world.get_settings()
        settings.synchronous_mode = True
        settings.fixed_delta_seconds = self.delta_t
        settings.no_rendering_mode = not self.rendering
        self.world.apply_settings(settings)

        vehicles = self.world.get_actors().filter("vehicle.*")
        self.vehicle = next(
            (actor for actor in vehicles if actor.attributes.get("role_name") == "hero"),
            vehicles[0] if vehicles else None,
        )
        if self.vehicle is None:
            library = self.world.get_blueprint_library()
            try:
                blueprint = library.find("vehicle.tesla.model3")
            except RuntimeError:
                blueprints = library.filter("vehicle.*")
                if not blueprints:
                    raise RuntimeError("CARLA has no vehicle blueprints available")
                blueprint = blueprints[0]
            if blueprint.has_attribute("role_name"):
                blueprint.set_attribute("role_name", "hero")
            spawn_points = self.world.get_map().get_spawn_points()
            if not spawn_points:
                raise RuntimeError("CARLA map has no vehicle spawn points")
            self.vehicle = self.world.try_spawn_actor(blueprint, spawn_points[0])
            if self.vehicle is None:
                raise RuntimeError("Could not spawn the bridge vehicle at spawn point 0")
            self.owns_vehicle = True

    def setup_sockets(self) -> None:
        linger = 0
        for sock in (self.pub, self.sub, self.sync):
            sock.setsockopt(zmq.LINGER, linger)
            sock.setsockopt(zmq.RCVTIMEO, int(self.timeout * 1000))
            sock.setsockopt(zmq.SNDTIMEO, int(self.timeout * 1000))

        self.pub.bind("tcp://127.0.0.1:5555")
        self.sub.setsockopt_string(zmq.SUBSCRIBE, "")
        for output_port in (5556, 5558, 5560):
            self.sub.connect(f"tcp://127.0.0.1:{output_port}")
        self.sync.connect("tcp://127.0.0.1:5557")

        self.sync.send_string("READY")
        response = self.sync.recv_string()
        if response != "READY":
            raise RuntimeError(f"OMNeT++ clock handshake failed: {response!r}")
        self._clock_ready = True
        # Give the PUB/SUB subscriptions time to propagate before the first frame.
        time.sleep(0.15)

    def setup_controls_window(self) -> None:
        if not self.controls_window:
            return
        pygame.init()
        self.control_screen = pygame.display.set_mode((430, 260))
        pygame.display.set_caption("Yes, CARLA CAN — Controls")
        self.control_font = pygame.font.SysFont(None, 25)

    def update_controls_window(self) -> None:
        if not self.controls_window or self.control_screen is None:
            return
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                self._controls_window_open = False
            elif event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                self._controls_window_open = False

        keys = pygame.key.get_pressed()
        self.commands["throttle"] = 0.35 if keys[pygame.K_w] or keys[pygame.K_UP] else 0.0
        self.commands["brake"] = 1.0 if keys[pygame.K_s] or keys[pygame.K_DOWN] else 0.0
        self.commands["steer"] = (
            -0.6 if keys[pygame.K_a] or keys[pygame.K_LEFT]
            else 0.6 if keys[pygame.K_d] or keys[pygame.K_RIGHT]
            else 0.0
        )
        self.commands["hand_brake"] = bool(keys[pygame.K_SPACE])

        self.control_screen.fill((25, 31, 42))
        lines = [
            "CARLA ↔ OMNeT++ keyboard controls",
            "W / ↑  Accelerate       S / ↓  Brake",
            "A / ←  Steer left      D / →  Steer right",
            "SPACE  Hand brake       ESC  Close controls",
            "",
            f"Throttle: {self.commands['throttle']:.2f}   Brake: {self.commands['brake']:.2f}",
            f"Steer: {self.commands['steer']:+.2f}      Hand brake: {self.commands['hand_brake']}",
            "Click this window before using the keys.",
        ]
        for index, line in enumerate(lines):
            color = (245, 245, 245) if index == 0 else (190, 205, 225)
            surface = self.control_font.render(line, True, color)
            self.control_screen.blit(surface, (18, 18 + index * 28))
        pygame.display.flip()

    def _signal_values(self, message) -> dict:
        values = {}
        for signal in message.signals:
            value = signal.initial
            if value is None or not math.isfinite(float(value)):
                value = 0
            if signal.minimum is not None:
                value = max(signal.minimum, value)
            if signal.maximum is not None:
                value = min(signal.maximum, value)
            values[signal.name] = value

        name = message.name
        if name == "THROTTLE":
            values["THROTTLE_signal"] = round(self.commands["throttle"] * 255)
        elif name == "BRAKE":
            values["BRAKE_signal"] = round(self.commands["brake"] * 255)
        elif name == "STEER":
            values["STEER_signal"] = round((self.commands["steer"] + 1.0) * 127.5)
        elif name == "REVERSE":
            values["REVERSE_signal"] = int(self.commands["reverse"])
        elif name == "HAND_BRAKE":
            values["HAND_BRAKE_signal"] = int(self.commands["hand_brake"])
        elif name == "MANUAL_TRANSMISSION":
            values["MANUAL_TRANSMISSION_signal"] = int(self.commands["manual_gear_shift"])
        elif name == "GEAR":
            values["GEAR_signal"] = self.commands["gear"]
        elif name in {"GNSS", "IMU_ACCEL", "IMU_GYRO", "IMU_COMPASS"}:
            # These values are sampled directly from the selected CARLA actor
            # and map at the current synchronous snapshot.
            if name == "GNSS":
                geo = self.world.get_map().transform_to_geolocation(
                    self.vehicle.get_location()
                )
                values.update(GNSS_LAT_signal=geo.latitude, GNSS_LON_signal=geo.longitude)
            elif name == "IMU_ACCEL":
                acceleration = self.vehicle.get_acceleration()
                values.update(IMU_ACCEL_X_signal=acceleration.x,
                              IMU_ACCEL_Y_signal=acceleration.y,
                              IMU_ACCEL_Z_signal=acceleration.z)
            elif name == "IMU_GYRO":
                angular = self.vehicle.get_angular_velocity()
                values.update(IMU_GYRO_X_signal=angular.x,
                              IMU_GYRO_Y_signal=angular.y,
                              IMU_GYRO_Z_signal=angular.z)
            else:
                yaw = self.vehicle.get_transform().rotation.yaw
                values["IMU_COMPASS_signal"] = (yaw + 90.0) % 360.0

            # CARLA's geolocation and dynamics can exceed the physical range
            # declared in the DBC; bound values before cantools encodes them.
            for signal in message.signals:
                value = float(values[signal.name])
                if not math.isfinite(value):
                    value = 0.0
                if signal.minimum is not None:
                    value = max(signal.minimum, value)
                if signal.maximum is not None:
                    value = min(signal.maximum, value)
                values[signal.name] = value
        return values

    def _publish_due_frames(self, sim_time: float) -> None:
        updated = []
        for message, cycle, next_due in self._schedule:
            if sim_time + 1e-9 >= next_due:
                payload = message.encode(self._signal_values(message)).hex()
                topic = ECU_BY_MESSAGE.get(message.name, "ADAS_ECU")
                self.pub.send_string(f"{topic} {message.frame_id} {payload}")
                while next_due <= sim_time + 1e-9:
                    next_due += cycle
            updated.append((message, cycle, next_due))
        self._schedule = updated

    def _harvest_frames(self, sim_time: float) -> None:
        seen_telemetry = set()
        while self.sub.poll(timeout=0, flags=zmq.POLLIN):
            wire_message = self.sub.recv_string(flags=zmq.NOBLOCK)
            fields = wire_message.split()
            if len(fields) != 3:
                continue
            try:
                can_id = int(fields[1])
                payload = bytes.fromhex(fields[2])
                decoded = self.db.decode_message(can_id, payload)
            except (ValueError, KeyError, cantools.database.errors.DecodeError):
                continue

            message = self.db.get_message_by_frame_id(can_id)
            if message.name in {"GNSS", "IMU_ACCEL", "IMU_GYRO", "IMU_COMPASS"}:
                # Several simulated receiving ECUs can report the same frame.
                # Print each distinct payload once per lockstep window.
                key = (can_id, payload)
                if key not in seen_telemetry:
                    print(json.dumps({"type": "bus_output", "sim_time": sim_time,
                                      "message": message.name, "can_id": can_id,
                                      "signals": decoded}), flush=True)
                    seen_telemetry.add(key)
            if message.name == "THROTTLE":
                self.controls["throttle"] = decoded["THROTTLE_signal"] / 255.0
            elif message.name == "BRAKE":
                self.controls["brake"] = decoded["BRAKE_signal"] / 255.0
            elif message.name == "STEER":
                self.controls["steer"] = decoded["STEER_signal"] / 127.5 - 1.0
            elif message.name == "REVERSE":
                self.controls["reverse"] = bool(decoded["REVERSE_signal"])
            elif message.name == "HAND_BRAKE":
                self.controls["hand_brake"] = bool(decoded["HAND_BRAKE_signal"])
            elif message.name == "MANUAL_TRANSMISSION":
                self.controls["manual_gear_shift"] = bool(decoded["MANUAL_TRANSMISSION_signal"])
            elif message.name == "GEAR":
                self.controls["gear"] = int(decoded["GEAR_signal"])
            if can_id in self._last_control_time:
                self._last_control_time[can_id] = sim_time

    def _apply_surviving_controls(self, sim_time: float) -> None:
        # A missing frame expires after two DBC periods. This also makes a
        # destroyed brake command fail safe after the last valid command ages out.
        for message_name, can_id in CONTROL_IDS.items():
            try:
                message = self.db.get_message_by_frame_id(can_id)
            except KeyError:
                continue
            cycle = (message.cycle_time or 100) / 1000.0
            last_seen = self._last_control_time[can_id]
            if last_seen is None or sim_time - last_seen > max(2 * cycle, self.delta_t):
                if message_name == "THROTTLE":
                    self.controls["throttle"] = 0.0
                elif message_name == "BRAKE":
                    self.controls["brake"] = 0.0
                elif message_name == "STEER":
                    self.controls["steer"] = 0.0
                elif message_name == "REVERSE":
                    self.controls["reverse"] = False
                elif message_name == "HAND_BRAKE":
                    self.controls["hand_brake"] = False
                elif message_name == "MANUAL_TRANSMISSION":
                    self.controls["manual_gear_shift"] = False
                elif message_name == "GEAR":
                    self.controls["gear"] = 1

        control = carla.VehicleControl()
        control.throttle = self.controls["throttle"]
        control.brake = self.controls["brake"]
        control.steer = self.controls["steer"]
        control.reverse = self.controls["reverse"]
        control.hand_brake = self.controls["hand_brake"]
        control.manual_gear_shift = self.controls["manual_gear_shift"]
        control.gear = self.controls["gear"]
        self.vehicle.apply_control(control)

    def run(self, max_ticks: int | None = None) -> None:
        try:
            self.setup_carla()
            self.setup_sockets()
            self.setup_controls_window()
            tick = 0
            while (max_ticks is None or tick < max_ticks) and self._controls_window_open:
                self.update_controls_window()
                sim_time = tick * self.delta_t
                target_time = (tick + 1) * self.delta_t
                self._publish_due_frames(sim_time)

                self.sync.send_string(f"{target_time:.9f}")
                response = self.sync.recv_string()
                if response != "OK":
                    raise RuntimeError(f"OMNeT++ lockstep failed at {target_time:.3f}s: {response!r}")

                self._harvest_frames(target_time)
                self._apply_surviving_controls(target_time)
                self.world.tick()
                tick += 1
                if tick % 50 == 0:
                    print(f"[co-sim] tick={tick} sim_time={target_time:.2f}s controls={self.controls}")
        finally:
            self.stop_omnet()
            self.teardown()

    def stop_omnet(self) -> None:
        if not self._clock_ready:
            return
        try:
            self.sync.send_string("STOP")
            response = self.sync.recv_string()
            if response != "BYE":
                print(f"[co-sim] OMNeT++ stop response: {response!r}")
        except zmq.ZMQError as exc:
            print(f"[co-sim] Could not stop OMNeT++ cleanly: {exc}")
        self._clock_ready = False

    def teardown(self) -> None:
        if self.world is not None and self.original_settings is not None:
            try:
                self.world.apply_settings(self.original_settings)
            except RuntimeError:
                pass
        if self.owns_vehicle and self.vehicle is not None:
            try:
                self.vehicle.destroy()
            except RuntimeError:
                pass
        for sock in (self.pub, self.sub, self.sync):
            sock.close(linger=0)
        self.context.term()
        if self.controls_window:
            pygame.quit()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dbc", type=Path, default=Path(__file__).parent / "data/carla.dbc")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2000)
    parser.add_argument("--fixed-delta", type=float, default=DEFAULT_DELTA)
    parser.add_argument("--throttle", type=float, default=0.7)
    parser.add_argument("--brake", type=float, default=1.0)
    parser.add_argument("--steer", type=float, default=0.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--rendering", action="store_true", help="enable CARLA rendering for camera sensors")
    parser.add_argument("--controls-window", action="store_true", help="open a keyboard controls window (WASD/arrows, space)")
    parser.add_argument("--max-ticks", type=int, default=None, help="stop after this many lockstep ticks")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    bridge = CarlaCanLockstepBridge(
        dbc_path=args.dbc,
        fixed_delta=args.fixed_delta,
        throttle=args.throttle,
        brake=args.brake,
        steer=args.steer,
        host=args.host,
        port=args.port,
        timeout=args.timeout,
        rendering=args.rendering,
        controls_window=args.controls_window,
    )
    try:
        bridge.run(max_ticks=args.max_ticks)
    except KeyboardInterrupt:
        print("\n[co-sim] stopped by user")


if __name__ == "__main__":
    main()
