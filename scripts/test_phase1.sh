#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:-unit}"

if [[ $# -gt 0 ]]; then
    shift
fi

case "$mode" in
    unit)
        if [[ $# -gt 0 ]]; then
            echo "Usage: $0 [unit | integration OUTPUT_DIRECTORY [SEED]]" >&2
            exit 2
        fi
        ;;
    integration)
        output_dir="${1:?Usage: $0 integration OUTPUT_DIRECTORY [SEED]}"
        seed="${2:-1}"
        ;;
    *)
        echo "Usage: $0 [unit | integration OUTPUT_DIRECTORY [SEED]]" >&2
        exit 2
        ;;
esac

cd "$project_root"

run_unit_checks() {
    python3 tools/export_omnet_manifest.py --check
    python3 tools/check_omnet_config.py
    python3 tools/validate_can_vectors.py
    python3 -m unittest tests.test_analyze_can_experiment -v
    make -C omnet test
}

write_validation_status() {
    local exit_code=$?
    printf '# Phase 1 validation status\n\n- Exit code: %s\n- Last stage: %s\n- Trace files are retained under `%s` for review.\n' \
        "$exit_code" "$stage" "$output_dir" > "$status_file"
    return "$exit_code"
}

run_case() {
    local name="$1"
    local config="$2"
    local observer="$3"
    local required_event="${4:-tx_success}"

    stage="running ${name}"
    ./scripts/run_omnet.sh --config "$config" --seed "$seed" \
        --observer "$observer" --output "${output_dir}/${name}"
    python3 tools/archive_can_run.py "${output_dir}/${name}" --config "$config" \
        --seed "$seed" --observer "$observer" \
        --omnet-root "${OMNETPP_ROOT:-${project_root}/.tools/omnetpp-6.0.2}"
    python3 tools/analyze_can_experiment.py "${output_dir}/${name}/events.jsonl" \
        --require-event "$required_event"
}

expect_unsupported_passive_error_flag() {
    local log_file="${output_dir}/no-ack-passive-limit.log"

    stage="checking that passive-only error flags fail explicitly"
    if ./scripts/run_omnet.sh --config NoAckPassiveLimit --seed "$seed" \
        --observer on \
        --output "${output_dir}/no-ack-passive-limit" > "$log_file" 2>&1; then
        echo "Expected the unsupported passive-only flag scenario to fail" >&2
        exit 1
    fi
    if ! grep -q 'unsupported passive-only error flag continuation' "$log_file"; then
        cat "$log_file" >&2
        echo "No-ACK limit failed for an unexpected reason" >&2
        exit 1
    fi
}

check_repeatability_and_observer_equivalence() {
    stage="checking repeatability and passive observer equivalence"
    python3 tools/compare_can_traces.py "${output_dir}/baseline/events.jsonl" \
        "${output_dir}/baseline-repeat/events.jsonl" --require-identical \
        --report "${output_dir}/repeatability-report.md"
    python3 tools/compare_can_traces.py "${output_dir}/baseline/events.jsonl" \
        "${output_dir}/observer-off/events.jsonl" --require-identical \
        --report "${output_dir}/observer-equivalence-report.md"
    cmp "${output_dir}/baseline/freshness.jsonl" "${output_dir}/observer-off/freshness.jsonl"
}

check_passive_runs() {
    stage="checking sampled timing and fields against independent reference rules"
    local name
    for name in baseline concurrent order-swap passive-error-fixture; do
        python3 tools/validate_passive_run.py "${output_dir}/${name}"
    done
    python3 tools/validate_passive_run.py "${output_dir}/queue-pressure" --freshness unchecked
    python3 tools/validate_passive_run.py "${output_dir}/missing-input" --freshness missing
}

run_unit_checks

if [[ "$mode" == "integration" ]]; then
    output_dir="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$output_dir")"
    mkdir -p "$output_dir"
    stage="starting OMNeT++ build"
    status_file="${output_dir}/validation_status.md"
    trap write_validation_status EXIT

    cp omnet/build/controller-fixture-events.jsonl "$output_dir/controller-fixture-events.jsonl"
    ./scripts/build_omnet.sh
    stage="build complete"

    run_case baseline Baseline on
    run_case baseline-repeat Baseline on
    run_case observer-off Baseline off
    run_case concurrent ConcurrentTraffic on
    run_case order-swap ArbitrationOrderSwap on
    run_case queue-pressure QueuePressure on
    run_case missing-input MissingInputControl on
    run_case passive-error-fixture PassiveErrorFixture on
    run_case no-ack NoAck on ack_error

    expect_unsupported_passive_error_flag
    check_repeatability_and_observer_equivalence
    check_passive_runs

    stage="checking arbitration order and bounded queue"
    python3 tools/check_phase1_scenarios.py "$output_dir"

    stage="all passive Phase 1 checks passed"
    echo "Phase 1 validation artifacts: ${output_dir}"
fi
