#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="${1:-General}"
source "$HOME/.venvs/opp-env/bin/activate"

opp_env run --no-isolated --add-extra-nix-packages zeromq -w "$HOME/omnetpp-workspace" \
    omnetpp-6.0.3 fico4omnet-20240124 \
    -c "cd '$SCRIPT_DIR' && ./run_omnet_in_env.sh '$CONFIG'"
