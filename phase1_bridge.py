#!/usr/bin/env python3
"""CARLA coordinator for the phase 1 bit-event CAN engine."""
import json
import carla
import zmq
from bridge_manager import CarlaCanLockstepBridge, ECU_BY_MESSAGE, parse_args

class Phase1Bridge(CarlaCanLockstepBridge):
    def setup_sockets(self):
        self.sync.setsockopt(zmq.LINGER, 0)
        self.sync.setsockopt(zmq.RCVTIMEO, int(self.timeout * 1000))
        self.sync.setsockopt(zmq.SNDTIMEO, int(self.timeout * 1000))
        self.sync.connect("tcp://127.0.0.1:5557")
        self._clock_ready = True

    def step(self, now, target):
        lines = [f"STEP {round(target * 1_000_000_000)}"]
        updated = []
        for message, cycle, next_due in self._schedule:
            if now + 1e-9 >= next_due:
                payload = message.encode(self._signal_values(message)).hex()
                ecu = ECU_BY_MESSAGE.get(message.name, "ADAS_ECU")
                lines.append(f"{ecu} {message.frame_id} {payload}")
                while next_due <= now + 1e-9:
                    next_due += cycle
            updated.append((message, cycle, next_due))
        self._schedule = updated
        self.sync.send_string("\n".join(lines))
        response = self.sync.recv_string().splitlines()
        if not response or response[0] != "OK":
            raise RuntimeError(f"Invalid step response: {response!r}")
        for line in response[1:]:
            frame_id, payload = line.split()
            can_id = int(frame_id)
            message = self.db.get_message_by_frame_id(can_id)
            decoded = message.decode(bytes.fromhex(payload))
            if message.name in {"GNSS", "IMU_ACCEL", "IMU_GYRO", "IMU_COMPASS"}:
                print(json.dumps({"type":"bus_output", "sim_time":target, "message":message.name, "can_id":can_id, "signals":decoded}), flush=True)
            mapping = {"THROTTLE": ("throttle", "THROTTLE_signal", 255), "BRAKE": ("brake", "BRAKE_signal", 255), "STEER": ("steer", "STEER_signal", 127.5)}
            if message.name in mapping:
                key, signal, divisor = mapping[message.name]
                self.controls[key] = decoded[signal] / divisor - (1 if key == "steer" else 0)
            elif message.name in {"REVERSE", "HAND_BRAKE", "MANUAL_TRANSMISSION", "GEAR"}:
                key = {"REVERSE":"reverse", "HAND_BRAKE":"hand_brake", "MANUAL_TRANSMISSION":"manual_gear_shift", "GEAR":"gear"}[message.name]
                self.controls[key] = decoded[message.name + "_signal"]
            if can_id in self._last_control_time:
                self._last_control_time[can_id] = target

    def run(self, max_ticks=None):
        try:
            self.setup_carla(); self.setup_sockets(); self.setup_controls_window()
            tick = 0
            while (max_ticks is None or tick < max_ticks) and self._controls_window_open:
                self.update_controls_window()
                self.step(tick * self.delta_t, (tick + 1) * self.delta_t)
                self._apply_surviving_controls((tick + 1) * self.delta_t)
                self.world.tick(); tick += 1
                if self.rendering:
                    transform = self.vehicle.get_transform()
                    forward = transform.get_forward_vector()
                    location = transform.location
                    camera = carla.Location(x=location.x - 8 * forward.x, y=location.y - 8 * forward.y, z=location.z + 3)
                    self.world.get_spectator().set_transform(carla.Transform(camera, carla.Rotation(pitch=-12, yaw=transform.rotation.yaw)))
                if tick % 50 == 0:
                    print(f"[co-sim] tick={tick} sim_time={tick * self.delta_t:.2f}s controls={self.controls}", flush=True)
        finally:
            self.stop_omnet(); self.teardown()

if __name__ == "__main__":
    args = parse_args()
    bridge = Phase1Bridge(dbc_path=args.dbc, fixed_delta=args.fixed_delta, throttle=args.throttle, brake=args.brake, steer=args.steer, host=args.host, port=args.port, timeout=args.timeout, rendering=args.rendering, controls_window=args.controls_window)
    try:
        bridge.run(args.max_ticks)
    except KeyboardInterrupt:
        print("[co-sim] stopped")
