#!/usr/bin/env bash
set -eo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
omnet="${OMNETPP_ROOT:-$root/.tools/omnetpp-6.0.2}"
source "$omnet/setenv" -q
cd "$root"
exec ./omnet/src/canlab -u Cmdenv -n omnet/ned -f omnet/simulations/cosim.ini "$@"
