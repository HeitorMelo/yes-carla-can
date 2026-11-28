# OMNeT++ Classical CAN experiment

This directory contains the standalone Phase 1 CAN simulation. It uses a
custom bit-event backend; see [the implementation plan](../integration/OMNET_CARLA_IMPLEMENTATION_PLAN.md).
See [Phase 1 progress and fidelity](docs/phase1-progress.md) for validation
evidence and the limits of the model.
CARLA, the external ECU repositories, and SocketCAN are not needed to run this experiment. The shared executable requires ZeroMQ development headers and library at build time.

## Install the headless toolchain

On Ubuntu 22.04, install the native build tools if they are missing:

```sh
sudo apt-get update
sudo apt-get install build-essential bison flex curl ca-certificates libzmq3-dev pkg-config
```

Then install the official OMNeT++ 6.0.2 core locally and build this project:

```sh
./scripts/install_omnet.sh
./scripts/build_omnet.sh
```

Run these commands from the repository root. The installer downloads a 74 MB
upstream archive, checks the release checksum, and builds a headless release
under `.tools/omnetpp-6.0.2`. It does not install globally. `OMNET_TOOL_ROOT`
can choose a different install parent; set `OMNETPP_ROOT` to the resulting
versioned directory when building/running, and use `OMNET_BUILD_JOBS` to limit
parallel compilation. Qt and optional IDE Python packages are not required.
The download requires network access.
OMNeT++ 6.0.2 identifies this distribution as its Academic Public License
edition, which excludes commercial use; review its bundled license for your
intended use.

## Run and inspect

```sh
./scripts/run_omnet.sh --config Baseline --seed 1 --output results/p1-baseline
```

Choose a new output directory for each run; the launcher refuses to overwrite
an existing event trace. The scripts are the intended entry points while the
model is being developed. Refer to the source READMEs for the currently
supported behavior and known limitations; this
model is not a claim of full ISO 11898 conformance. The passive observer has
only an input gate; `--observer off` disables observation for an equivalence
check. `wire-observations.jsonl` records observed sample times, intervals,
field positions, stuffing, and decoded identifiers. `freshness.jsonl` records
the gateway's fresh/stale brake-command observations. It is a diagnostic sink,
not a vehicle-control fallback.

Error-flag overlap and passive-only flag continuation remain unsupported; the latter stops the simulator with an explicit error. The earlier Phase 1 branch contains validation fixtures and scripts, which are not included here.

## CARLA integration with the Phase 1 CAN engine

The `phase1_bridge.py` coordinator uses the bit-event engine through a ZeroMQ
request/reply step protocol. Install `libzmq3-dev` and `pkg-config` to build the
OMNeT++ executable, and install `pyzmq`, `pygame`, `carla`, and `cantools` in
the CARLA Python environment. Set `OMNETPP_ROOT` when the local OMNeT++ 6.0.2
installation is outside this checkout. Build with `./scripts/build_omnet.sh`.

Start CARLA separately, then run `./scripts/run_phase1_cosim.sh` and, from
another terminal, `python phase1_bridge.py --rendering --controls-window`.
Use `--max-ticks 100` for a bounded smoke run. Focus the controls window and
use WASD/arrows and space. The coordinator selects or creates a vehicle and,
with `--rendering`, follows it with the spectator camera. Close the controls
window or interrupt the coordinator to stop OMNeT++; stop CARLA separately.

Each request contains all DBC frames due at the start of the 20 ms window;
OMNeT++ simulates arbitration and bits and returns the frames accepted by the
actuator gateway before the boundary. Frames still in flight remain in the
engine for the next step. DBC periods are sampled on 20 ms boundaries rather
than independently scheduled inside each window, and delivery timestamps in
the console are harvest-window times. Motion telemetry is sampled from CARLA
actor state, not attached sensor devices. The same model restrictions described
above apply; this integration is not evidence of full CAN conformance. The
legacy SocketCAN modules are not part of this launch.

### Local verification and outstanding problems

On 2026-11-27, a local run used CARLA 0.9.15, OMNeT++ 6.0.2 and Python 3.9.
A 100-tick run (two simulated seconds) completed, returned GNSS and IMU frames,
and applied a received throttle command. A second 100-tick run completed with
rendering and the controls window enabled. A separate request carrying two CAN
frames returned both IDs and their original payloads. The CAN core checks,
reference vectors, manifest check and configuration check were reported as passing on the source branch. These are
smoke checks; they do not establish end-to-end determinism, timing accuracy,
attack behavior, keyboard response under load, or full protocol conformance.

- The bit-event engine retains the Phase 1 limitations documented above,
  including unsupported overlapping error flags and passive-only continuation.
- Periodic DBC releases are rounded to coordinator step boundaries. Returned
  frames have window timestamps, not their precise simulated delivery times.
  The single gateway receives valid deliveries; this setup does not model
  per-ECU application subscriptions or event-driven CARLA sensors.
- The current bridge blocks waiting for a request; it has no server-side idle
  timeout or automatic recovery if its peer disappears. Start OMNeT++ before
  the coordinator and terminate any leftover processes after a failed run.
- CARLA reported a segmentation fault when interrupted after each local run.
  OMNeT++ exited after the coordinator's `STOP` message. The CARLA shutdown
  fault has not been diagnosed.
- The OMNeT++ executable now links against ZeroMQ, including for standalone
  configurations built with `scripts/build_omnet.sh`.
