#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
: "${FICO4OMNET_ROOT:?Install fico4omnet-20240124 in the opp_env workspace first}"

cd "$SCRIPT_DIR"
pkg-config --exists libzmq || {
    echo "libzmq development files are missing; install libzmq3-dev and pkg-config." >&2
    exit 1
}
read -r -a zmq_cflags <<< "$(pkg-config --cflags libzmq)"
read -r -a zmq_libs <<< "$(pkg-config --libs libzmq)"

opp_makemake -f --deep -o carla_can_bridge \
    "${zmq_cflags[@]}" \
    -I"$FICO4OMNET_ROOT/src" \
    -L"$FICO4OMNET_ROOT/src" \
    -lFiCo4OMNeT "${zmq_libs[@]}"
make -j"$(nproc)" MODE=release
