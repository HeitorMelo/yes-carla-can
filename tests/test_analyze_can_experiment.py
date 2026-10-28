"""Negative and boundary checks for the trace analysis contract."""

from __future__ import annotations

import unittest

from tools.analyze_can_experiment import check_trace, check_attack_stops_early, compare_attack_to_baseline


def event(name: str, time_ns: int, ecu: str = "Chassis_ECU", *,
          tec_before: int = 0, tec_after: int = 0,
          rec_before: int = 0, rec_after: int = 0,
          state: str = "error_active", **extra) -> dict:
    row = {
        "time_ns": time_ns,
        "run": "case",
        "seed": 1,
        "ecu": ecu,
        "frame_uid": f"{ecu}:601:0",
        "attempt": 1,
        "can_id": 0x601,
        "dlc": 4,
        "payload_hex": "80000000",
        "event": name,
        "reason": "fixture",
        "field": "",
        "bit_index": -1,
        "tec_before": tec_before,
        "tec_after": tec_after,
        "rec_before": rec_before,
        "rec_after": rec_after,
        "state": state,
        "queue_depth": 0,
    }
    row.update(extra)
    return row


class TraceAnalysisTests(unittest.TestCase):
    @staticmethod
    def bus_off_trace() -> list[dict]:
        rows = []
        for i, before in enumerate(range(0, 256, 8), start=1):
            after = before + 8
            rows.append(event("bit_error", i, tec_before=before, tec_after=after,
                              state="bus_off" if after == 256 else "error_passive" if after > 127 else "error_active"))
        rows.append(event("bus_off", 32, tec_before=248, tec_after=256, state="bus_off"))
        rows.append(event("tx_blocked_bus_off", 33, tec_before=256, tec_after=256, state="bus_off"))
        return rows

    def test_accepts_bus_off_at_256_and_subsequent_block(self) -> None:
        trace = self.bus_off_trace()
        self.assertEqual(check_trace(trace), [])

    def test_rejects_fabricated_bus_off_below_threshold(self) -> None:
        trace = [event("bus_off", 10, tec_before=56, tec_after=64, state="error_active")]
        errors = check_trace(trace)
        self.assertTrue(any("bus-off requires TEC transition to 256" in error for error in errors))

    def test_rejects_bus_off_with_no_counter_evidence(self) -> None:
        trace = [{"time_ns": 10, "run": "case", "ecu": "Chassis_ECU", "event": "bus_off"}]
        errors = check_trace(trace)
        self.assertTrue(any("requires a complete integer TEC/REC snapshot" in error for error in errors))

    def test_rejects_transmit_after_bus_off(self) -> None:
        trace = self.bus_off_trace()[:-1]
        trace.append(event("tx_success", 40, tec_before=256, tec_after=256, state="bus_off"))
        errors = check_trace(trace)
        self.assertTrue(any("after bus-off" in error for error in errors))
        self.assertTrue(any("transmit success must reduce TEC" in error for error in errors))

    def test_rejects_arbitration_error_counter_change(self) -> None:
        trace = [event("arbitration_lost", 5, tec_before=0, tec_after=8, state="error_active",
                       field="Arbitration", bit_index=4)]
        errors = check_trace(trace)
        self.assertTrue(any("arbitration loss changed TEC" in error for error in errors))

    def test_accepts_receiver_error_counter_increment(self) -> None:
        trace = [event("receiver_error", 5, tec_before=0, tec_after=0,
                       rec_before=127, rec_after=128, state="error_passive")]
        self.assertEqual(check_trace(trace), [])

    def test_rejects_backwards_simulation_time(self) -> None:
        trace = [
            event("tx_queue_admit", 20),
            event("tx_attempt", 19),
        ]
        errors = check_trace(trace)
        self.assertTrue(any("time moves backward" in error for error in errors))

    def test_accepts_competing_transmitters_and_arbitration_loss(self) -> None:
        winner = event("tx_attempt", 1, "Powertrain_ECU", can_id=0x600, frame_uid="p:600:0")
        loser = event("tx_attempt", 1, "Chassis_ECU", can_id=0x601, frame_uid="c:601:0")
        lost = event("arbitration_lost", 25, "Chassis_ECU", field="Arbitration",
                     bit_index=3, tec_before=0, tec_after=0)
        complete = event("tx_success", 200, "Powertrain_ECU", can_id=0x600,
                         frame_uid="p:600:0", tec_before=0, tec_after=0)
        self.assertEqual(check_trace([winner, loser, lost, complete]), [])

    def test_rejects_arbitration_claim_without_live_contender(self) -> None:
        winner = event("tx_attempt", 1, "Powertrain_ECU", can_id=0x600, frame_uid="p:600:0")
        loser = event("tx_attempt", 1, "Chassis_ECU", can_id=0x601, frame_uid="c:601:0")
        winner_done = event("tx_success", 100, "Powertrain_ECU", can_id=0x600,
                            frame_uid="p:600:0", tec_before=0, tec_after=0)
        loser_fault = event("bit_error", 110, "Chassis_ECU", tec_before=0, tec_after=8,
                            can_id=0x601, frame_uid="c:601:0", state="error_active")
        retry = event("tx_attempt", 150, "Chassis_ECU", can_id=0x601,
                      frame_uid="c:601:0", attempt=2, tec_before=8, tec_after=8)
        stale_loss = event("arbitration_lost", 200, "Chassis_ECU", can_id=0x601,
                           frame_uid="c:601:0", attempt=2, field="Arbitration", bit_index=3,
                           tec_before=8, tec_after=8)
        errors = check_trace([winner, loser, winner_done, loser_fault, retry, stale_loss])
        self.assertTrue(any("simultaneous attempts" in error for error in errors))

    def test_attack_pair_requires_targeted_fault_and_unaffected_traffic(self) -> None:
        baseline = [
            event("tx_attempt", 1, "Chassis_ECU", tec_before=0, tec_after=0),
            event("tx_success", 100, "Chassis_ECU", tec_before=0, tec_after=0),
            event("actuator_update", 100, "ActuatorGateway", can_id=0x601),
        ]
        attack = [
            event("tx_attempt", 0, "Chassis_ECU", field="", bit_index=-1),
            event("attack_drive", 0, "Chassis_ECU", field="Data", bit_index=19),
        ]
        for i, before in enumerate(range(0, 256, 8), start=1):
            after = before + 8
            attack.append(event("bit_error", i, "Chassis_ECU", tec_before=before, tec_after=after,
                                state="bus_off" if after == 256 else "error_passive" if after > 127 else "error_active",
                                field="Data", bit_index=19))
        attack.append(event("bus_off", 32, "Chassis_ECU", tec_before=248, tec_after=256,
                            state="bus_off", field="Data", bit_index=19))
        attack.append(event("tx_success", 34, "Powertrain_ECU", can_id=0x600,
                            frame_uid="p:600:1", tec_before=0, tec_after=0))
        errors, metrics = compare_attack_to_baseline(baseline, attack, "Chassis_ECU", 0x601, "Powertrain_ECU")
        self.assertEqual(errors, [])
        self.assertEqual(metrics["attack_victim_successes"], 0)
        self.assertEqual(metrics["unaffected_successes_after_bus_off"], 1)

    def test_early_stop_requires_actuator_recovery(self) -> None:
        trace = [
            event("tx_attempt", 1, "Chassis_ECU", frame_uid="c:601:0"),
            event("attack_drive", 2, "Chassis_ECU", frame_uid="c:601:0", field="Data", bit_index=19),
            event("bit_error", 3, "Chassis_ECU", frame_uid="c:601:0", field="Data", bit_index=19,
                  tec_before=0, tec_after=8),
            event("tx_attempt", 10, "Chassis_ECU", frame_uid="c:601:0", attempt=2,
                  tec_before=8, tec_after=8),
            event("tx_success", 20, "Powertrain_ECU", can_id=0x600,
                  frame_uid="p:600:0", tec_before=0, tec_after=0),
            event("tx_success", 30, "Chassis_ECU", frame_uid="c:601:0", attempt=2,
                  tec_before=8, tec_after=7),
            event("actuator_update", 30, "ActuatorGateway", can_id=0x601),
        ]
        errors, metrics = check_attack_stops_early(trace, "Chassis_ECU", 0x601, "Powertrain_ECU")
        self.assertEqual(errors, [])
        self.assertEqual(metrics["target_deliveries_after_fault"], 1)

    def test_attack_pair_rejects_unmatched_drive(self) -> None:
        baseline = [event("tx_success", 1, "Chassis_ECU", tec_before=0, tec_after=0)]
        attack = self.bus_off_trace()
        attack.insert(0, event("tx_attempt", 0, "Chassis_ECU"))
        attack.insert(0, event("attack_drive", 0, "Chassis_ECU", field="Data", bit_index=18))
        attack[0]["can_id"] = 0x601
        for row in attack:
            if row["event"] == "bit_error":
                row["field"] = "Data"
                row["bit_index"] = 19
        errors, _ = compare_attack_to_baseline(baseline, attack, "Chassis_ECU", 0x601, "Powertrain_ECU")
        self.assertTrue(any("does not match" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
