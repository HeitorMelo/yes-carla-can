# End-to-end plan: CAN simulation and passive timing analysis in OMNeT++ and CARLA

## Outcome and delivery boundary

Deliver this work in exactly two implementation phases. The plan does not implement or execute a CAN attack. Any attack implementation is explicitly outside this plan and must not be added as a deliverable, test requirement, or simulation profile.

1. **OMNeT++ only:** build, run, and validate a standalone multi-ECU CAN network and a passive wire-timing observer. It must run headlessly without CARLA, ZeroMQ, SocketCAN, or an external process feeding traffic. The phase ends with reproducible baseline traffic, validated bit-position/timing observations, and defensive monitoring checks.
2. **CARLA integration:** connect that validated network to timestamped CARLA/controller inputs and demonstrate correct command delivery, freshness handling, and defensive monitoring in a repeatable driving scenario.

### Experiment scope and safety boundary

Both phases are offline software simulations. Phase 1 uses synthetic ECUs and the OMNeT++ event model; phase 2 couples the validated network to CARLA's simulated vehicle. Neither phase connects to a physical vehicle, physical CAN interface, external network, or third-party system. Treat any vehicle response as a simulator result only. The observer is read-only: it estimates timing and bit positions from resolved bus samples or recorded traces, and has no interface for transmitting, modifying, or scheduling bits. Focus evaluation on protocol behavior, detection, resilience to benign missing or stale inputs, and stated model limitations. Any later attack implementation is outside the scope of this plan.

This document is an implementation plan, not a claim that the attached C++ examples compile or that the required FiCo4OMNeT features already exist. Proposed commands and files below are interfaces to implement. No dependency repository needs to be changed to follow this plan.

## Starting point in this repository

| Existing component | Reuse or adjustment |
|---|---|
| `data/carla.dbc` | Preserve the existing IDs, payload layouts, signal names, and cycle times. Introduce ECU ownership through a separate experiment DBC and a validated runtime manifest. |
| `can_network/dbc.py` | Reuse DBC validation and encoding contracts. Extract the reusable codec from CARLA and transport imports. |
| `can_network/network.py` | Contains useful control conversions, but currently combines SocketCAN transport, decoding, and retained actuator state. Separate those concerns for phase 2. |
| `vehicle_controls_module.py` | Currently sends periodic traffic using wall-clock time. Add a simulation-time controller path for experiments. |
| `CARLA_client_module.py`, `gui/world.py`, `gui/keyboard_control.py` | Currently contain world ticks, actor lifecycle, and direct control paths. The experiment runner must own ticking and actuation. |
| `sensors/` | Reuse sensor encoding after normalizing units and collecting callbacks by CARLA frame number. |
| `adapters/can_ecu_adapter.py`, `integration/can_ecu_ownership.json` | Existing one-way translation of C simulator IDs `0x104`–`0x106` into CARLA DBC controls. Keep this as a separate integration mode. |
| `1_up_environment.sh` | Launches CARLA and a bidirectional `vcan0`/`vcan1` gateway. Add a separate experiment launcher so that traffic cannot bypass OMNeT++. |
| `CAN-bus-simulator`, `doip-sim-ecu-dsl`, `ecu-simulator` | Keep as dependencies at their existing revisions. Preserve local edits. They are not prerequisites for the standalone OMNeT++ network. |

There is currently no checked-in OMNeT++ project in the parent repository. The existing `vcan` path transports complete frames; it cannot establish bit-level arbitration, CRC/ACK behavior, or controller error confinement.

## Corrected assumptions and design decisions

### CAN model and passive observation fidelity

- Start with **Classical CAN, standard 11-bit data frames, DLC 0–8, one ideal shared bus, 500 kbit/s**. A nominal bit occupies 2 microseconds. Exclude CAN FD, extended identifiers, analog voltage behavior, oscillator drift, and propagation-dependent sampling from the first implementation. Document unsupported remote/overload-frame cases and reject unsupported inputs rather than silently approximating them.
- DBC ownership describes application producers and consumers. It does not instantiate OMNeT++ modules, implement arbitration, authenticate a sender, or govern ACK generation. `Vector__XXX` represents an unspecified receiver; it does not create a physical broadcast or ACK policy. Configure actual controllers and their receive/ACK behavior separately.
- Different identifiers compete through arbitration. A node that transmits recessive and reads dominant in the arbitration field loses arbitration without a transmit error. After arbitration, an unexpected bus value can be a bit error, subject to protocol exceptions.
- Treat bus-off as a **controller state**. Losing the Chassis controller removes all its transmissions and bus participation, not just message `0x601`. A receiver losing fresh brake commands is a separate application failure policy.
- Implement error-active, error-passive, and bus-off behavior from a documented rule table. Error-passive begins when TEC or REC exceeds 127; bus-off occurs when TEC exceeds 255. Include successful-traffic adjustments, relevant exception rules, retransmission, and error-passive transmit suspension. Do not promise that every experiment takes exactly 32 errors. [Microchip fault-confinement reference](https://onlinedocs.microchip.com/oxy/GUID-4D282FC5-82FC-4934-8BAD-D4A5D8422E6C-en-US-7/GUID-7D190839-A6DB-403E-9C9A-71C7F894A376.html).
- Keep the timing observer passive. It may report the observed bit interval and decoded field/index when supported by the wire samples, but it must not select a target for intervention, drive the bus, or expose an injection hook. Validate it against codec-generated golden traces and independent recorded fixtures.
- Controller error confinement remains part of protocol simulation and unit validation. Exercise counter/state transitions using documented rule tables and prerecorded event fixtures; do not add a bus participant that deliberately creates errors or a scenario whose success criterion is forcing a controller bus-off.

### FiCo4OMNeT is a candidate backend, not an assumed capability

The [official FiCo4OMNeT repository](https://github.com/CoRE-RG/FiCo4OMNeT) describes CAN frame traffic and priority arbitration with simulation adaptations. That description alone does not establish an exposed bit bus or complete fault-confinement implementation. Its [installation instructions](https://github.com/CoRE-RG/FiCo4OMNeT/blob/master/INSTALL) identify OMNeT++ 6.0.2 as the prerequisite; begin compatibility work there and pin the exact successful revisions.

Use an early, time-bounded source audit and prototype. If FiCo provides the required mechanisms, wrap those verified APIs. If it does not, implement a focused bit-event CAN backend in the parent OMNeT++ project. Keep FiCo optional for comparison. **Do not run independent FiCo and custom arbitration/error engines for the same bus**, and do not modify the existing dependency repositories. The snippets in the attachment use unverified class names, gates, setters, and parameters; replace them with interfaces established by the audit.

### Time and transport

Phase 1 uses OMNeT++ simulation time exclusively. Phase 2 uses a 20 ms CARLA macro step, while OMNeT++ continues processing individual CAN bits and events inside each macro step. Application periods come from the DBC; periods shorter than or not divisible by 20 ms remain exact OMNeT++ release events.

Use integer nanoseconds in manifests, traces, and the bridge protocol. OMNeT++ has fixed-point simulation time and explicit ordering for simultaneous events; define event priorities instead of relying on module insertion order. [OMNeT++ simulation manual](https://doc.omnetpp.org/omnetpp/manual/index.html).

Use **one sequenced ZeroMQ request/reply transaction for authoritative step inputs and outputs**. PUB/SUB can optionally stream a UI view, but cannot carry required experiment data because subscribers can miss messages. [ZeroMQ PUB/SUB behavior](https://zguide.zeromq.org/docs/chapter5/).

## Shared architecture and application contract

```text
Phase 1
  Synthetic ECU applications + scripted driver/actuator sink
                         |
                  controller TX queues
                         |
            CAN controllers <-> resolved bit bus
                         |
      passive timing observations + state/counter traces

Phase 2
  CARLA state/sensors -> coordinator + controller policy
                         | sequenced STEP request
                  OMNeT++ bridge scheduler
                         |
             same ECU applications/controllers/bus/observer
                         | STEP_RESULT with complete payloads
  CARLA actuation <- timestamped receive events + freshness policy
```

Use four initial transmitting ECU applications:

| ECU | DBC messages | Initial scheduling |
|---|---|---|
| `Powertrain_ECU` | THROTTLE `0x600`, REVERSE `0x603`, MANUAL_TRANSMISSION `0x606`, GEAR `0x607` | Preserve 100/200/500/200 ms periods. |
| `Chassis_ECU` | BRAKE `0x601`, STEER `0x602`, HAND_BRAKE `0x604` | Preserve 100/100/200 ms periods. |
| `ADAS_ECU` | AUTOPILOT `0x605`, GNSS `0x60E`, COLLISION `0x60F`, LANE_INVASION `0x610`, IMU_ACCEL `0x611`, IMU_GYRO `0x612`, IMU_COMPASS `0x613`, RADAR_TARGET `0x614` | Periodic messages use DBC periods; collision, lane, and radar releases are explicit events. AUTOPILOT remains an application mode command, not direct CARLA autopilot authority. |
| `Body_ECU` | DOORS `0x60C`, GENERAL_LIGHTS `0x60D` | On-change events; optional explicit heartbeat only if declared as an experiment override. |

Add a fifth **ActuatorGateway** receive endpoint to make delivery semantics explicit. It has a real controller that can ACK correctly received traffic and an application that reconstructs delivered commands. It is the future CARLA actuation boundary. Additional receivers are permitted; their acceptance filters do not redefine DBC metadata as physical routing. Duplicate physical receptions are legitimate and are identified by receiver, not counted as duplicate transmissions.

Create `data/carla_multi_ecu.dbc` from the existing DBC with the ownership changes in the second attachment. Preserve the original DBC for the existing application. Add `integration/omnet/ecu_manifest.json` with explicit controller instances, consumers, queue sizes, phases, receiver filters, and timeout policies. Generate its message layout/period section from the experiment DBC and validate it against a checked-in hash. C++ can read the exported manifest without requiring a DBC parser at runtime. Do not reuse the C simulator's ownership registry for this distinct ID space.

## Phase 1 — standalone OMNeT++ network and passive timing observer

### P1.1 — establish the toolchain and backend gate

1. Record OS/compiler, OMNeT++ version, build flags, FiCo revision if used, and any INET or other dependencies actually required by its build metadata. Build the smallest official example headlessly before attempting a custom topology. Install only this phase's dependencies; CARLA and ZMQ are unnecessary.
2. Inspect FiCo's bus, controller, frame definitions, arbitration, receive path, error handling, and counters. Produce `omnet/docs/backend-audit.md` listing the actual symbols and evidence for each needed behavior.
3. Run two-node arbitration and passive trace-observation probes. Check that the observer reports sampled timing without changing transmission or delivery behavior.
4. Limit investigation to one focused working day. Select the verified FiCo backend or the focused custom backend, then freeze the choice in an architecture decision. A successful frame-level example alone does not pass this gate.

**Deliverables:** reproducible build entry point, pinned dependency manifest, capability matrix, selected backend, and a minimal headless run. Preserve licenses and attribution when reusing external model code.

### P1.2 — define configuration, traffic, and results before the model grows

1. Add the multi-ECU DBC, generator/validator, and runtime manifest. Define one producer per normal message, payload vectors, release phase, queue bounds, arbitration policy within each controller, and explicit event releases for zero-cycle messages.
2. Implement synthetic applications in OMNeT++: scripted controls, periodic telemetry, event-driven body/sensor messages, and a receive-side actuator sink. Use simulation-time timers and absolute next-release timestamps, never wall-clock polling.
3. Distinguish application release, TX queue admission, each wire attempt, valid bus completion, receiver acceptance, and actuator update. A retry keeps the application frame identity but receives a new attempt number.
4. Define a common event record: run/seed, integer time, ECU, frame UID, attempt, CAN ID, DLC/payload, event/reason, bit field/index where relevant, TEC/REC before and after, controller state, and queue depth. Record sender metadata for analysis only; a CAN receiver does not gain authenticated sender identity from it.
5. Emit a run manifest with configuration/DBC/build hashes and output hashes. Produce summary CSV/JSON even when detailed bit tracing is disabled.

**Deliverables:** reusable traffic fixtures and output contract, with no CARLA process required.

### P1.3 — implement or verify the shared bus and controller state machines

Develop in the following order, using the selected backend:

1. **Bit serialization:** SOF, arbitration/control fields, DLC/data, CRC sequence, applicable bit stuffing, CRC delimiter, ACK slot/delimiter, EOF, and intermission. Retain the original payload and mappings between logical fields and stuffed-wire positions. Use independent known vectors for CRC and stuffing checks.
2. **Shared bus resolution:** at each modeled bit, collect all drivers before resolving dominant/recessive and sampling. Define drive, resolve, sample, and resulting state-transition ordering. A single global resolver can avoid order-dependent node updates. Model synchronized ideal sampling first; a channel delay and `bitLength` alone cannot implement this.
3. **Arbitration:** simultaneous SOF releases compete bit by bit. A loser becomes a receiver and retains its queued frame for a later attempt. Same-ID transmitters remain active until another rule distinguishes them.
4. **Receive and ACK:** valid receivers check the frame and drive ACK where appropriate. Report no receiver/no ACK correctly. Deliver payloads only after the modeled validity checks finish; do not publish a frame at SOF or acknowledge an application delivery prematurely.
5. **Error detection and recovery:** represent protocol error rules needed by the supported model; active/passive flags, delimiters, retransmission, and error-passive suspension. Validate controller-state transitions with independent rule-table fixtures, not an active bus intervention scenario. Define explicit behavior for unsupported protocol cases.
6. **Error confinement:** implement a reviewed table of counter changes and exceptions, state transitions, and successful-traffic adjustments. Include arbitration loss and ACK-slot exceptions so legitimate bus behavior is not misclassified as a bit error. Compare it with a separate reference table in tests.
7. **Bus-off:** stop the affected controller from transmitting, ACKing, or emitting error flags. Make application enqueue requests after bus-off observable as rejected or held according to a declared queue policy. Disable automatic recovery for the primary experiment. If recovery is later implemented, model its required idle observations and controller reinitialization as a separate profile.

Keep the protocol engine in C++ units that can be tested without CARLA. NED modules expose configuration, connections, and statistics. Do not claim full ISO conformance: publish the supported subset, exercised rules, and remaining limitations.

**Deliverables:** a deterministic bus/controller implementation, supported-feature matrix, reference traces, and bounded resource usage under overload.

### P1.4 — implement a read-only wire-timing observer

1. Consume resolved bus samples or completed trace records without changing bus state. Estimate bit intervals and, when the sample stream supports it, map observed positions to decoded frame fields and logical bit indices.
2. Validate timing and position estimates against codec-generated golden traces covering stuffing boundaries, DLC values, and arbitration outcomes. Keep expected metadata in the test harness; the observer itself must derive results from its input samples.
3. Record uncertainty or unsupported cases explicitly. Do not add target selection, dominant/recessive drive controls, intervention schedules, or retry-following logic.
4. Add `Baseline`, `ConcurrentTraffic`, `PassiveErrorFixture`, and `MissingInputControl` profiles. Fault-state fixtures are prerecorded/controller-level validation data and do not inject bus bits.

**Deliverables:** a passive timing/field observer, reference traces, and regression checks that prove observation does not alter transmitted bits, deliveries, or controller state.

### P1.5 — verification, experiment runner, and phase exit

| Check | Required evidence |
|---|---|
| Frame correctness | Golden payload/CRC/stuffing traces, boundary DLC values, malformed-input rejection, and correct ACK behavior. |
| Arbitration | Simultaneous `0x600`/`0x601`: `0x600` wins, loser retries, no arbitration-loss TEC penalty. Swap release/module initialization order and preserve the result. |
| Queueing/load | Multiple messages from one ECU, busy-bus releases, bounded queues, deterministic overflow policy, and no fabricated successful deliveries. |
| Timing observer | Estimated bit intervals and field/bit positions match golden traces, including stuffing boundaries; observer output has no effect on the bus. |
| Confinement rules | Table-driven boundary/exception checks, successful TX/RX adjustments, transition through error-passive, suspension, and bus-off. |
| Controller rules | Prerecorded rule-table fixtures verify error-counter boundaries and state transitions without an active bus intervention scenario. |
| Observation-only control | Baseline wire samples, successful deliveries, and controller counters are identical with the observer enabled and disabled. |
| Missing-input control | An explicitly absent or stale application input exercises receiver freshness policy without adding CAN error traffic. |
| Repeatability | Identical seed/configuration produces identical ordered protocol events and summary values, excluding wall-clock performance fields. |

Implement proposed entry points:

```bash
./scripts/build_omnet.sh
./scripts/run_omnet.sh --config Baseline --seed 1 --output results/p1-baseline
./scripts/run_omnet.sh --config ConcurrentTraffic --seed 1 --output results/p1-concurrent
python3 tools/validate_passive_run.py results/p1-baseline
./scripts/test_phase1.sh integration results/p1-validation
```

The first three commands build and run the standalone network; the remaining commands validate its observations and run the full suite. The runner must return nonzero for configuration errors, failed assertions, missing required outputs, or observer-validation failures. Benchmark short runs before choosing experiment durations; bit-level events at 500 kbit/s can be expensive. Enable detailed traces around selected observation windows, retain counters/delivery summaries throughout, and record achieved simulation speed.

**Phase 1 exit:** a fresh environment can execute headless network scenarios without CARLA/ZMQ/vcan, inspect passive timing results, and reproduce the same protocol trace with the observer on or off. Archive the supported-feature matrix and reference traces. Start phase 2 only after this gate passes.

## Phase 2 — deterministic CARLA integration

### P2.1 — implement the transport and scheduler without CARLA first

Use an OMNeT++ bridge scheduler around simulation execution, with one ZeroMQ context and one authoritative REP endpoint bound by OMNeT++ on a configurable loopback address, initially port 5557. The Python coordinator connects using REQ. Keep socket handling out of individual ECU modules.

Protocol v1 uses structured JSON and integer nanoseconds:

- `HELLO`: protocol version, run/session ID, model/DBC/configuration hashes, bit rate, step size, and capabilities. Response is `READY` only after configuration compatibility passes.
- `STEP`: run/session ID, monotonically increasing step ID, `start_ns`, `end_ns`, and a complete batch of application input changes/event releases. Each input identifies its logical ECU, effective time, message or signal values, and unique input UID.
- `STEP_RESULT`: matching identifiers, committed horizon, ordered successful receiver deliveries with **full payloads**, relevant TX/failure events, controller state transitions, and summary counters. A `TX_COMPLETE` report alone is not a receiver delivery.
- `STOP`/structured `ERROR`: deterministic completion or failure reason and last committed step.

Define each simulation window as **`[start_ns, end_ns)`**. Inputs effective at its start are installed before events at that time execute. Use a reserved boundary event/scheduler mechanism to reach `end_ns` and pause before domain events stamped exactly at that boundary; those remain for the next step. Specify priorities so the horizon cannot close before earlier events finish, and so simultaneous releases are visible before bit resolution. Frames crossing a boundary retain their controller/bus state.

The scheduler may wait for the next request while the simulation is explicitly paused at a boundary. Ordinary `handleMessage()` handlers must return without waiting for the peer. Do not poll a socket every 0.5 ms of simulated time to discover frames whose release timing matters at 2 microseconds.

Use finite wall-clock transport timeouts, zero/bounded shutdown linger, message-size limits, and structured parse errors. A timeout pauses/fails the run; it never changes simulated CAN traffic. Cache the last committed response so a reconnect/retry with the same step ID and same request hash cannot execute the step twice. Reject conflicting duplicates, skipped steps, stale sessions, backward times, invalid ECU ownership, bad DLC, and timestamps outside the declared interval. Predeclare within-window input events; reject any that arrive after their window was committed.

Validate with a synthetic Python coordinator before introducing CARLA. Replaying the same inputs in standalone and stepped execution must yield the same network trace, including frames and error flags crossing step boundaries. Exercise empty steps, multiple intra-step releases, exact-boundary events, malformed requests, reconnect, and timeout behavior.

### P2.2 — separate codecs, application policy, and transport

1. Extract transport-independent DBC encoding/decoding from `CAN_Network`. Preserve throttle/brake scaling and centered steering. Check unknown IDs before decoding; report malformed payloads without crashing the run.
2. Keep the existing SocketCAN backend for the existing application. Add an OMNeT++ backend/coordinator path that queues logical ECU inputs and consumes committed receive events. Never expose outgoing raw commands directly on a bus that the CARLA actuator also reads.
3. Keep periodic release ownership inside OMNeT++ in both phases. At a macro-step boundary, the coordinator updates each ECU application's latched signal values; its existing periodic timers encode/send at exact DBC release times. Zero-cycle sensor/body events are explicit timestamped releases. Do not generate the same periodic frame in Python and C++.
4. Refactor the control policy so desired commands come from either a scripted driver, a route controller, or keyboard input sampled at a boundary. Commands pass through the ECU/bus/ActuatorGateway path before actuation. A route controller may compute desired commands; CARLA autopilot must not independently apply them to the vehicle.
5. Normalize sensor units against the DBC before encoding. In particular, verify CARLA gyro/compass angular units against the degree-based fields, check radar conversion, handle saturation, and define a deterministic policy when multiple radar targets share one message ID. Record both source measurement time and CAN release time.
6. Introduce a dedicated headless experiment runner rather than requiring Tk/pygame and the interactive GUI. The UI can later observe committed experiment state.

### P2.3 — make the co-simulation causal and reproducible

Use CARLA 0.9.15 initially, with a matching Python API, synchronous mode, and `fixed_delta_seconds = 0.02`. Configure valid physics substeps, one world-tick owner, fixed scenario/weather/spawn parameters, and seeds. If Traffic Manager is used for background traffic, configure its synchronous mode and seed. Restore saved settings on teardown. [CARLA synchrony and time-step documentation](https://carla.readthedocs.io/en/0.9.15/adv_synchrony_timestep/).

For each macro step, execute this order:

1. Start at `t_k` with the CARLA snapshot/sensor batch already available and the actuator control committed at that boundary from the preceding network window. Use explicit initial controls for the first interval. Compute desired commands and update logical ECU inputs using only information available at or before `t_k`.
2. Submit the input batch in `STEP(k, t_k, t_k + dt)` and wait for the committed `STEP_RESULT`. Stage its delivered commands without applying them to the current physics interval.
3. Call `world.tick()` once to advance CARLA from `t_k` to `t_k + dt` using the control already committed at `t_k`.
4. At `t_k + dt`, commit the staged gateway deliveries and evaluate freshness using their exact network receive timestamps. Apply the resulting control for the **following** physics interval. Preserve the ordered delivery history even if only the final command state is held for that interval.
5. Collect the resulting snapshot and scheduled sensor callbacks by CARLA frame ID; those measurements become inputs for the following network window. Event callbacks from this tick are not sent retroactively into a committed window.

This is a causal partitioned coupling with controls committed at macro-step boundaries. A delivery inside `[t_k, t_k + dt)` first affects physics at `t_k + dt`; it cannot change the physics interval that contains its reception. This adds between approximately zero and one macro step of boundary delay after a delivery. Record desired-input time, network receive time, boundary commit time, and the CARLA frame/interval using the applied control separately. Keep exact network timestamps and compare 20 ms with a smaller step when evaluating sensitive consequences. Events stamped exactly at a horizon belong to the following network window under the defined half-open convention.

Define required sensor streams and expected frames from their configured cadence. Wait only for expected scheduled samples; collision/lane events and empty radar detections must not be treated as mandatory callbacks every tick. Use bounded waits and deterministic missing-sample rules. Measure repeatability with trajectory tolerances; do not promise bit-identical CARLA physics across hardware.

### P2.4 — define missing-command behavior before choosing the driving demo

The current receiver retains prior control values. Preserve **sample-and-hold** as the first documented experiment policy; optionally add a watchdog policy with a declared timeout, for example three 100 ms brake periods. Never set brake to zero merely because the current 20 ms window contains no brake frame.

Policies are application configuration, not CAN behavior:

- `hold_last`: retain each channel's last valid value until an explicit declared limit, or indefinitely if the profile says so. A pending brake request can fail to arrive while the receiver holds the earlier zero-brake value.
- `watchdog`: after a defined freshness deadline, enter an explicitly configured fallback control state. Evaluate a protective braking fallback as a mitigation comparison if desired.

Do not read privileged OMNeT++ bus-off state to force CARLA brake failure. Use that state for the experiment log; actuation must follow delivered commands and the declared receiver policy. A diagnostic-based fallback requires its own modeled diagnostic delivery.

Use an initial straight-road scenario with a fixed vehicle, spawn, target speed, weather, and scripted brake-request time. Keep all simulated CAN traffic on the normal ECU application path. Exercise healthy delivery and a benign missing/stale-command control to verify the declared receiver freshness policy and defensive fallback. Measure command age, applied controls, vehicle response, and minimum obstacle distance; do not create a bus intervention or an attack-caused vehicle response.

### P2.5 — isolate the experiment path and package execution

1. Add `scripts/run_cosim.sh` and `scripts/stop_cosim.sh`, or an equivalent supervisor, with tracked child PIDs, readiness probes, timeouts, and reliable cleanup. Do not use a fixed sleep as the only readiness check.
2. Launch CARLA offscreen using the already updated flags. Offscreen rendering and no-rendering mode have different sensor consequences; choose the scenario's required sensor set explicitly.
3. For authoritative experiments, run the new coordinator and OMNeT++ only. Do not launch the existing independent controller/client pair or the bidirectional CAN gateway through the default environment script.
4. If candump/IDS visibility is needed, mirror only committed deliveries to a dedicated observation interface such as `vcan2`. Keep it one-way and tag mirrored frames. No observation gateway may feed raw requests back into actuation or OMNeT++. SocketCAN timing/error flags are not the source of truth for the bit model.
5. Archive synchronized network events, CARLA frame/time, desired/delivered/applied controls, freshness/fallback events, pose/speed, collision metrics, seeds, tool revisions, and scenario hashes.

Proposed user-facing commands:

```bash
./scripts/run_cosim.sh --scenario command_freshness --profile healthy --seed 1 --output results/p2-healthy
./scripts/run_cosim.sh --scenario command_freshness --profile missing-input --seed 1 --output results/p2-missing-input
python tools/analyze_cosim.py results/p2-healthy results/p2-missing-input
```

### P2.6 — integration verification and phase exit

- Synthetic stepped execution matches standalone network execution for the same input trace, with exact bit/controller events.
- Encode → simulated TX → valid gateway RX → decode preserves ID, DLC, and payload. Corrupted/failed attempts never masquerade as delivered controls.
- Periodic values continue to be held between releases; zero-cycle events do not become continuous traffic. Busy-bus traffic and frames crossing 20 ms boundaries remain queued and timestamped correctly.
- A causality regression changes a command in STEP `k`: the physics interval `t_k` to `t_{k+1}` still uses the prior committed command, and only `t_{k+1}` to `t_{k+2}` uses the new delivery. Assert that later sensor observations never enter an already committed network window.
- Healthy input is delivered and applied under the specified controller policy; the missing-input control exercises the declared freshness/fallback behavior without generating CAN error traffic.
- Startup failures, stale/duplicate step requests, missing sensors, peer shutdown, and CTRL-C produce bounded cleanup and a recorded failure reason.
- Repeat healthy/missing-input pairs across a small fixed seed set, then compare 20 ms and a smaller supported macro step. Report protocol determinism separately from trajectory tolerance and coupling sensitivity.
- Existing SocketCAN behavior still has a supported launch path. The three nested dependencies retain their original revisions and local contents.

**Phase 2 exit:** a documented clean launch produces healthy and missing-input archives, plots observer timing and gateway command age beside vehicle response, and demonstrates the declared freshness/fallback behavior using only simulator inputs.

## File layout to implement

```text
omnet/
  src/can/                  # Selected bus/controller backend and frame codec
  src/apps/                 # Synthetic ECU apps, ActuatorGateway, passive observer
  src/bridge/               # Phase-2 scheduler and protocol endpoint
  ned/                      # ECU, bus, gateway, and experiment networks
  simulations/              # Baseline, traffic, and missing-input configurations
  tests/                    # Protocol vectors, rule tables, and trace regressions
  docs/                     # Backend audit and fidelity/architecture decisions
data/carla_multi_ecu.dbc
integration/omnet/
  ecu_manifest.json
  protocol-v1.schema.json
  scenarios/                # Network and paired driving experiment parameters
can_network/codec.py         # Transport-independent conversions
cosim/
  protocol.py
  coordinator.py
  carla_runner.py
  controllers.py
  sensor_collector.py
  actuator_policy.py
scripts/build_omnet.sh
scripts/run_omnet.sh
scripts/run_cosim.sh
scripts/stop_cosim.sh
tools/export_omnet_manifest.py
tools/analyze_can_experiment.py
tools/analyze_cosim.py
```

Paths are proposed boundaries; adapt naming to the actual build layout. Keep generated results, build outputs, downloaded tools, and external library source out of normal source commits. Commit small reference traces and fixtures needed to review correctness.

## Assignment for up to four implementation agents

One agent must own integration and the shared contracts. Parallelize after defining those contracts; each source file has one active owner.

| Agent | Phase 1 ownership | Phase 2 ownership |
|---|---|---|
| **A — integration lead** | Backend audit, toolchain pinning, NED/build/run configuration, manifest contract, integration and phase-exit review. | Protocol schema, bridge scheduler integration, launch/cleanup, compatibility review, phase-exit review. |
| **B — CAN engine** | Bit serialization, wired bus resolution, arbitration, controller/ACK/error/confinement implementation. | Preserve engine behavior under step boundaries; integrate external input latches and event export. |
| **C — applications and observer** | Synthetic ECU applications, actuator sink, passive timing observer. | CARLA coordinator/driver policy, codec reuse, actuator freshness and fallback behavior. |
| **D — validation and analysis** | Independent reference vectors/rule tables, scenario assertions, trace comparison, result analysis and performance measurements. | Sensor collection/units, fake-peer and replay checks, paired driving scenarios and trajectory/causality reports. |

**Execution order:**

1. A and B resolve P1.1. Meanwhile C specifies passive-observation fixtures and D prepares independent expected traces; neither relies on unverified FiCo APIs.
2. Freeze frame, controller-state, manifest, and event interfaces. B develops P1.3, C develops P1.2/P1.4 against those interfaces, D builds checks, and A assembles the runnable network.
3. Integrate continuously, then complete P1.5. All agents review failures; phase 1 is a complete deliverable before any CARLA dependency is introduced.
4. Freeze protocol v1 and window-boundary semantics. A/B implement scheduler/engine integration; C implements the coordinator and actuator policy; D implements sensor collection and synthetic peer checks.
5. Pass standalone-versus-stepped replay checks, then integrate CARLA, run paired scenarios, and complete P2.6.

Use one integration branch and separate agent branches/worktrees if implementation begins. Merge a coherent dependency layer before consumers depend on it. Do not let two agents concurrently change common protocol schemas or controller state definitions.

## Effort, risks, and completion checklist

Provisional effort for four available agents: roughly **2–4 calendar weeks for phase 1** if a custom bit/controller backend is needed, and **1–2 additional weeks for phase 2** after its gate passes. These are planning ranges, not commitments; the backend audit, protocol exceptions, and reference validation determine the actual schedule. A verified reusable backend could shorten phase 1 substantially.

| Risk | Planned response |
|---|---|
| FiCo lacks bit/error hooks or build compatibility | Time-bound the audit; select the custom backend and keep optional frame-level comparison separate. |
| Simplified error rules create misleading state results | Independent rule-table tests and short reviewed prerecorded traces; do not claim attack feasibility from controller-state fixtures. |
| Observer output is mistaken for a bus action | Keep its interface read-only and verify observer-on/off protocol trace equivalence. |
| Fine-grained events make long runs slow | Benchmark first, bound durations, narrow bit tracing, and optimize only after trace equivalence checks. |
| CARLA or raw vcan traffic bypasses the network | Single tick/control authority and a separate launcher with an isolated authoritative path. |
| Macro-step timing changes the claimed consequence | Preserve exact network timestamps; document partitioned coupling and compare smaller macro steps. |
| Configuration drifts between languages | Generated manifest, shared fixtures/schema, startup hashes, and standalone/stepped replay comparison. |

- [x] Phase 1 builds and runs without CARLA, ZMQ, or vcan.
- [x] The selected CAN model has a documented supported subset and controller confinement fixtures.
- [x] Arbitration, no-ACK error detection, and retries are evidenced in simulator traces; state transitions and bus-off blocking are evidenced in controller-level fixture traces.
- [x] The passive observer matches reference timing/position traces and cannot modify bus state.
- [ ] Phase 2 preserves those network semantics across step boundaries.
- [ ] Only delivered commands and explicit freshness policy determine CARLA actuation.
- [ ] Healthy and missing-input runs have reproducible configuration and synchronized evidence.
- [ ] Existing dependency repositories and their local edits remain untouched.

## Local integration smoke-run findings (2026-11-26)

The `carla-omnet-integration` branch was manually exercised on Ubuntu/Linux with
CARLA 0.9.15, OMNeT++ 6.0.2, FiCo4OMNeT revision
`c4e15633ee5ab66d32b86a3b8d81835c9970ac3a`, and the repository's `n4s_env`
(Python 3.9, including `pyzmq`). A 100-tick lockstep run completed and streamed
GNSS and IMU telemetry. A continuous run also advanced until deliberately
stopped. This is a launch/transport smoke check, not evidence of protocol
fidelity, deterministic replay, or a performance benchmark.

### Reproduced local build and launch

The checked-in `carla_can_bridge/build.sh` expects `opp_env`, Nix, OMNeT++
6.0.3, and FiCo4OMNeT `20240124`. That environment was unavailable on the test
machine. FiCo4OMNeT's checked-in `INSTALL` instead specifies OMNeT++ 6.0.2, so
the bridge was built locally against the project's `.tools/omnetpp-6.0.2` and
the FiCo4OMNeT source tree under `.tools/FiCo4OMNeT`:

```bash
source .tools/omnetpp-6.0.2/setenv -q
git clone https://github.com/CoRE-RG/FiCo4OMNeT.git .tools/FiCo4OMNeT
git -C .tools/FiCo4OMNeT checkout c4e15633ee5ab66d32b86a3b8d81835c9970ac3a
cd .tools/FiCo4OMNeT
make makefiles
make -j4 MODE=release
cd ../..
```

On this host, `pkg-config --cflags libzmq` returned an `-isystem` option that
OMNeT++ 6.0.2's `opp_makemake` rejects. The successful bridge build omitted
those unnecessary system include flags (ZeroMQ headers were in `/usr/include`):

```bash
source .tools/omnetpp-6.0.2/setenv -q
cd carla_can_bridge
opp_makemake -f --deep -o carla_can_bridge \
  -I"$PWD/../.tools/FiCo4OMNeT/src" \
  -L"$PWD/../.tools/FiCo4OMNeT/src" \
  -lFiCo4OMNeT -lzmq
make -j4 MODE=release
```

For a visible continuous run, start CARLA in windowed mode, start the bridge
model with a larger simulation-time limit, then start the coordinator with
rendering and its controls window enabled. Use separate terminals:

```bash
# Terminal 1, repository root: the startup script's default map is Town10HD_Opt.
./carla-0-9-15/CarlaUE4.sh -windowed -ResX=1280 -ResY=720 \
  -quality-level=Low -nosound -carla-rpc-port=2000

# Terminal 2, working directory: carla_can_bridge.
source ../.tools/omnetpp-6.0.2/setenv -q
export LD_LIBRARY_PATH="$PWD/../.tools/FiCo4OMNeT/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./carla_can_bridge -u Cmdenv \
  -n ".:$PWD/../.tools/FiCo4OMNeT/src" -f omnetpp.ini -c NoAttack \
  --sim-time-limit=9000000s

# Terminal 3, repository root: stdout is the live bus/telemetry log.
/home/robocin2d/miniconda3/envs/n4s_env/bin/python -u bridge_manager.py \
  --timeout 30 --throttle 0 --brake 0 --rendering --controls-window
```

The controls window uses W/Up for throttle, S/Down for brake, A/Left and
D/Right for steering, and Space for handbrake. The checked-in OMNeT++ config
has a 1000-second default limit; without the command-line override it stops
before an unbounded coordinator session. The 9,000,000-second override stays
within OMNeT++ 6.0.2's representable simulation-time range. Close the
coordinator cleanly first so it can send `STOP` to OMNeT++, then stop CARLA.

The original `bridge_manager.py` had fixed command-line controls and no
controls window. An optional Pygame controls window was added for this local
integration workflow. `--rendering` is required for visible rendering; without
it the coordinator sets CARLA's `no_rendering_mode`. The standard
`1_up_environment.sh` SocketCAN keyboard sender is not a drop-in controller
for this bridge because it publishes to `vcan`, while the integration routes
frames through ZeroMQ and OMNeT++.

### Remaining setup issues

- Make the bridge build reproducible from a clean machine: reconcile the
  `opp_env` OMNeT++ 6.0.3 pin with FiCo4OMNeT's OMNeT++ 6.0.2 instructions, and
  handle `pkg-config` flags unsupported by `opp_makemake`.
- The local smoke run used the `NoAttack` configuration. It does not validate
  attack behavior, bit-level CAN conformance, stale-command policy, or
  repeatability across seeds.
- CARLA and OMNeT++ need explicit coordinated cleanup. A coordinator exit
  sends `STOP` to OMNeT++; the CARLA process is stopped separately.
