# Phase 1 progress: CAN simulation and passive timing analysis

The passive Phase 1 implementation and its validation suite are complete for
the supported subset described below. The active network uses synthetic ECUs,
an ideal CAN bus, a read-only timing observer, and a gateway freshness monitor.
Existing legacy driver source remains outside the active topology and the
revised experiment scope.

## Validation evidence

The source Phase 1 branch reported the following results on Ubuntu 22.04.5, GCC 11.4 and OMNeT++ 6.0.2. Its validation suite and fixtures are not included in this branch; these historical results do not validate later bridge changes.

The suite builds the model, verifies independent CRC/stuffing reference vectors
and controller rule fixtures, then runs baseline, repeat, observer-off,
concurrent traffic, release-order swap, queue-pressure, missing-input,
controller-fixture, and no-ACK controls. It also checks that the unsupported
passive-only error flag case fails explicitly.

In the seed-1, three-second baseline, 245 complete frames and 20,801 observed
bits matched independent reference construction; samples within each frame
were 2,000 ns apart. The repeated run and observer-off event traces were
byte-identical, including complete resolved frame samples, controller records,
and deliveries. Changing release order preserved the 245-frame delivery
sequence; queue pressure rejected 70 requests without delivering any rejected
frame. The missing-input control reported a stale command 150 ms after the last
delivery. A no-ACK control recorded errors and retries without a delivery.

The controller fixture records error-active/passive transitions, bus-off, and a
blocked request in a separate controller-level trace. It is not evidence of an
end-to-end bus-off experiment. Each run writes hashes and toolchain metadata;
`results/` is local output and is not committed. The reference vectors and controller fixture remain available on the source Phase 1 branch. A clean copy of the source was built and passed the same suite with a
fresh OMNeT++ installation on the same host. The installer verified the cached
official 6.0.2 archive before building it; this did not test the download or a
separate operating-system installation.

## Supported subset and limitations

| Area | Supported behavior and limits |
|---|---|
| Frame format | Classical CAN standard data frames with DLC 0–8, serialization, stuffing, CRC, and incremental receive checks. Extended and remote frames and CAN FD are unsupported. |
| Bus and arbitration | Ideal synchronized samples and dominant-wins arbitration. Propagation and oscillator effects are not modeled. Same-ID differing frames terminate explicitly. |
| Observation | Read-only sample timestamps, intervals, field/index classification, stuffing, and decoded ID. Observer-on/off protocol traces match. |
| Error handling | No-ACK and active error flag paths are exercised. Passive-only error flag continuation is unsupported and fails explicitly; overlapping flags and error recovery timing are simplified. Controller confinement is checked using fixtures, not a full bus-off run. |
| Applications | Synthetic periodic and one-shot events, bounded queues, and freshness diagnostics. The gateway records stale commands; Phase 2 must implement any actuation fallback. |
| Configuration | DBC and manifest are checked against the NED traffic definitions. Fixture traffic is configured in NED rather than dynamically loaded from the manifest. |

These results do not establish complete ISO 11898 conformance or behavior on
physical vehicles. Phase 2 may use the validated healthy-traffic and freshness
path, and must retain the stated protocol limitations.
