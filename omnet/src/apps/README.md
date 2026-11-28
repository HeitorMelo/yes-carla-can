# OMNeT++ application modules

`CanLabNetwork.ned` connects four synthetic ECU applications, an
`ActuatorGateway`, the bit-level bus, and a read-only `PassiveObserver`. The
observer has one input gate and no output gate. It records resolved samples,
their simulation times, and decoded field positions in
`wire-observations.jsonl`; it does not participate in bus resolution.

Applications release frames using simulation-time timers. Specifications use
`CAN_ID,period_ns,phase_ns,DLC,payload_hex`; a zero period produces one event.
The missing-input control limits chassis releases to one per message. The
gateway records fresh brake deliveries and a stale event after 150 ms without
a new brake delivery. These records are diagnostics, not a CARLA actuator
policy.

The bus resolves simultaneous FIFO heads bit by bit. Arbitration losers retain
their frames for retry without a counter penalty. Identical same-ID frames can
proceed together; differing same-ID payloads are rejected as unsupported.
Receiver validation precedes delivery at EOF, and the trace records the full
resolved frame, including intermission. A no-ACK control checks error reporting
and retries without any successful delivery.

The model uses ideal synchronized sampling and simplified error-flag timing.
It stops with an explicit error when only passive error flags would be present,
because continuation of the remaining receivers is not implemented. Overlap
of error flags, analog effects, extended identifiers, remote frames, and CAN
FD are outside the supported subset. The core controller-state fixture is
separate from the simulation and does not establish full protocol conformance.

Existing legacy driver source remains in the tree, but the active topology
does not instantiate it or connect a drive gate. The revised Phase 1 runner
uses only the passive network profiles in `omnetpp.ini`.
