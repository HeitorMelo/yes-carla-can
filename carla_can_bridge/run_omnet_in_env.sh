#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="${1:-General}"
: "${FICO4OMNET_ROOT:?Install fico4omnet-20240124 in the opp_env workspace first}"

cd "$SCRIPT_DIR"
export LD_LIBRARY_PATH="$FICO4OMNET_ROOT/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./carla_can_bridge -u Cmdenv -n ".:$FICO4OMNET_ROOT/src" -f omnetpp.ini -c "$CONFIG"
