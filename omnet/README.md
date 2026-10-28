# OMNeT++ Classical CAN experiment

This directory contains the standalone Phase 1 CAN simulation. It uses a
custom bit-event backend; see [the FiCo4OMNeT audit](docs/backend-audit.md)
and [the implementation plan](../integration/OMNET_CARLA_IMPLEMENTATION_PLAN.md).
See [Phase 1 progress and fidelity](docs/phase1-progress.md) for validation
evidence and the limits of the model.
CARLA, ZeroMQ, the external ECU repositories, and SocketCAN are not needed to
run this experiment.

## Install the headless toolchain

On Ubuntu 22.04, install the native build tools if they are missing:

```sh
sudo apt-get update
sudo apt-get install build-essential bison flex curl ca-certificates
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
python3 tools/validate_passive_run.py results/p1-baseline
./scripts/test_phase1.sh integration results/p1-validation
```

Choose a new output directory for each run; the launcher refuses to overwrite
an existing event trace. The scripts are the intended entry points while the
model and validation are being developed. Refer to the source READMEs and
test results for the currently supported behavior and known limitations; this
model is not a claim of full ISO 11898 conformance. The passive observer has
only an input gate; `--observer off` disables observation for an equivalence
check. `wire-observations.jsonl` records observed sample times, intervals,
field positions, stuffing, and decoded identifiers. `freshness.jsonl` records
the gateway's fresh/stale brake-command observations. It is a diagnostic sink,
not a vehicle-control fallback.

The following checks need a C++17 compiler and Python 3, but not CARLA or
OMNeT++:

```sh
make -C omnet test
python3 tools/validate_can_vectors.py
python3 tools/export_omnet_manifest.py --check
python3 tools/check_omnet_config.py
```

See [validation artifacts](tests/README.md) for the scope and limitations of
these checks. Error-flag overlap and passive-only flag continuation remain
unsupported; the latter stops the simulator with an explicit error.
