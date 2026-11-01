# Phase 1 readability refactor

The goal is to make the existing Classical CAN simulation easier to review and
maintain while preserving its supported behavior, interfaces, output formats,
and documented fidelity limits.

1. **Done:** Simplify the CAN codec, receiver, controller, and passive observers. Group
   related operations and name protocol boundaries clearly.
2. **Done:** Organize the OMNeT++ bus adapter and applications around their simulation
   responsibilities, keeping event ordering and ownership explicit.
3. **Done:** Simplify the Phase 1 validation scripts while retaining their scenarios and
   failure checks.
4. **Done:** Review the combined changes, build both the standalone core and OMNeT++
   model, and run the Phase 1 validation suite. Check Phase 1b compatibility
   where it uses shared scripts or configuration.

The Phase 1 and Phase 1b validation suites passed. Event, wire-observation,
and freshness traces matched the original implementation across seven Phase 1
scenarios. A separate comparison also matched 4,096 serialized frames,
including their CRC offsets and bit metadata.

Refactoring does not add protocol features or establish additional CAN or
physical-layer conformance. Existing Phase 1 limitations remain documented in
[Phase 1 progress](phase1-progress.md).
