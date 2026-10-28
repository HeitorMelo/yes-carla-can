#!/usr/bin/env bash
set -eo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
omnet_dir="${OMNETPP_ROOT:-${project_root}/.tools/omnetpp-6.0.2}"
config=Baseline
seed=1
output=""
observer=true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config) config="${2:?missing config}"; shift 2 ;;
        --seed) seed="${2:?missing seed}"; shift 2 ;;
        --output) output="${2:?missing output directory}"; shift 2 ;;
        --observer) observer="${2:?expected on or off}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 --config NAME --seed NUMBER --output DIRECTORY [--observer on|off]"
            exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$output" || ! "$seed" =~ ^[0-9]+$ ]]; then
    echo "An output directory and a nonnegative integer seed are required." >&2
    exit 2
fi
case "$observer" in on|true) observer=true ;; off|false) observer=false ;; *) echo "Observer must be on or off" >&2; exit 2 ;; esac
if [[ ! -x "${project_root}/omnet/src/canlab" ]]; then
    echo "CAN simulation has not been built; run ./scripts/build_omnet.sh first." >&2
    exit 1
fi
if [[ -e "${output}/events.jsonl" ]]; then
    echo "Refusing to overwrite ${output}/events.jsonl; choose another output directory." >&2
    exit 1
fi
mkdir -p "$output"
output="$(cd "$output" && pwd)"
source "${omnet_dir}/setenv" -q
cd "$output"
"${project_root}/omnet/src/canlab" -u Cmdenv -c "$config" \
    -n "${project_root}/omnet/ned" "${project_root}/omnet/simulations/omnetpp.ini" \
    --seed-set="$seed" "--**.bus.experimentSeed=$seed" "--**.observer.enabled=$observer" --result-dir="$output"
echo "Results saved in ${output}"
