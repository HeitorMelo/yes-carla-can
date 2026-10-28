#!/usr/bin/env bash
set -eo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
omnet_dir="${OMNETPP_ROOT:-${project_root}/.tools/omnetpp-6.0.2}"
if [[ ! -f "${omnet_dir}/Makefile.inc" ]]; then
    echo "OMNeT++ is not configured at ${omnet_dir}; run ./scripts/install_omnet.sh first." >&2
    exit 1
fi
source "${omnet_dir}/setenv" -q
cd "${project_root}/omnet/src"
opp_makemake -f --deep -u Cmdenv -o canlab
make -j"${OMNET_BUILD_JOBS:-$(nproc)}" MODE=release
echo "Built ${project_root}/omnet/src/canlab"
