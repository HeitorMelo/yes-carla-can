# CAN backend decision

## FiCo4OMNeT source audit

Audited [CoRE-RG/FiCo4OMNeT](https://github.com/CoRE-RG/FiCo4OMNeT) at commit `c4e15633ee5ab66d32b86a3b8d81835c9970ac3a` against the Phase 1 requirements in `integration/OMNET_CARLA_IMPLEMENTATION_PLAN.md`. The audit copy was kept outside this project; the dependency repositories in the workspace were not changed.

| Capability | Source evidence | Decision |
|---|---|---|
| Arbitration | `src/fico4omnet/fico4omnet-doc.ned` explicitly describes centralized `CanBusLogic` permission instead of arbitration on the physical layer. `CanBusLogic::registerForArbitration` records the CAN ID and controller. | Does not supply sampled wired-bus arbitration or same-ID bit competition. |
| Serialized bits and stuffing | `CanTrafficSourceAppBase::calculateStuffingBits` estimates stuffing from a configured percentage and frame length. | Does not provide the stuffed wire image needed for passive field and timing observations. |
| Error injection | `CanPortInput::generateReceiveError` chooses an error position and error kind randomly, then schedules an `ErrorFrame`; `CanBusLogic::handleErrorFrame` distributes it. | A frame-level error event is available, but it does not by itself produce the required bus-level mismatch and controller fault-confinement result. |
| Error confinement | Source searches for transmit/receive error counters, error-active/passive and bus-off state did not find an implementation in the CAN model. | Insufficient evidence for modeling the target's progression to bus-off. |

**Selected backend:** an independent Classical CAN bit-event model in this repository, driven by OMNeT++. FiCo4OMNeT is not linked into the same simulated bus. The source audit resolves the backend decision; successful probes in the custom model still need to establish correctness. Its supported protocol subset and remaining limitations must be recorded separately from this audit.

## Toolchain

The official [OMNeT++ 6.0.2 core distribution](https://omnetpp.org/download-items/omnetpp/omnetpp-602.html) matches the version specified by FiCo's installation instructions and provides a headless Cmdenv runtime. The installation script downloads the 6.0.2 core archive, checks the official release MD5 (`28822b9210e57b946f8bd09c701b855d`), and builds it locally with Qtenv and OpenSceneGraph disabled. The checksum verifies the release download against the publisher's published checksum; it is not a signed provenance check.

On Ubuntu 22.04, the build in this workspace uses GCC 11.4 and GNU Make. See the Phase 1 README for setup and execution. Optional OMNeT++ IDE Python packages and graphical runtimes are not required for the headless experiment.

The official `Tictoc1` sample was built and run successfully with Cmdenv and a 1 ms simulation-time limit on this workspace. This confirms the installed kernel and runtime can execute an upstream example; it does not validate the CAN model.
