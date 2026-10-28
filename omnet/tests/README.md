# Phase 1 validation artifacts

`can_reference_vectors.json` contains fixed standard Classical CAN data-frame
vectors, including CRC and stuffing expectations. The Python reference checker
independently reconstructs those values. The C++ core checks the codec,
incremental receiver, arbitration, malformed frames, controller boundaries,
and the passive field decoder across multiple IDs and DLC values.

`controller_rule_fixture.csv` provides controller-level operations and expected
state checkpoints, including error-passive, bus-off, and blocked transmission.
The core test writes a step-by-step replay to
`omnet/build/controller-fixture-events.jsonl`. This is a controller fixture,
not an end-to-end error-flag or bus-off simulation.

From the repository root, run the core checks with:

```sh
./scripts/test_phase1.sh unit
```

With OMNeT++ installed, run the full passive suite into a new output directory:

```sh
./scripts/test_phase1.sh integration results/p1-validation
```

The suite builds the model and runs baseline, repeated baseline, observer-off,
concurrent traffic, reversed release order, bounded queue, missing input,
passive controller fixture, and no-ACK profiles. A separate no-ACK limit
checks that the unsupported passive-only flag case fails with the expected
diagnostic. The suite validates sampled fields,
stuffing and intervals against independent Python reference rules; requires
byte-identical baseline event traces with observation on and off; checks
freshness and arbitration behavior; and archives configuration, source,
binary, output, and toolchain hashes. Controller fixture events and Markdown
comparison reports are written beside the run directories.

The model uses an ideal bus. Passive-only error flag continuation and flag
overlap are not modeled, and the former fails explicitly. Same-ID conflicting
transmissions are rejected. Results do not establish full ISO conformance or
behavior of real vehicle hardware.
