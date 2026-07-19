#!/usr/bin/env bash
# Integration test: verify idle_minutes values cause backend shutdown at correct time
# Tests: idle_minutes=0,1,2,3 via real gatekeeper binary
# Key: must connect to port to trigger backend spawn, then measure idle timeout
set -euo pipefail

EVIDENCE_DIR=".omo/evidence/idle-minutes-integration-test"
mkdir -p "${EVIDENCE_DIR}"
LOG="${EVIDENCE_DIR}/test-run.log"
: > "${LOG}"

BIN="./build/linux/x86_64/debug/gatekeeper"
PORT_BASE=39990  # base port, will add idle value

# idle_minutes => [sleep_sec, min_expected, max_expected]
declare -A SL=( [0]=30 [1]=120 [2]=180 [3]=240 )
declare -A MN=( [0]=25  [1]=55 [2]=115 [3]=175 )
declare -A MX=( [0]=35  [1]=90 [2]=150 [3]=210 )

kill_all() {
    pkill -9 -f gatekeeper 2>/dev/null || true
    pkill -9 -f "sleep 30" 2>/dev/null || true
    pkill -9 -f "sleep 180" 2>/dev/null || true
    pkill -9 -f "sleep 240" 2>/dev/null || true
}

run_test() {
    local idle=$1
    local ss=${SL[$idle]} lo=${MN[$idle]} hi=${MX[$idle]}
    echo "=== idle_minutes=${idle} ===" | tee -a "${LOG}"

    # Compute port for this idle value
    local PORT=$((PORT_BASE + idle))
    # Clean previous
    kill_all; sleep 1
    cfg=$(mktemp)
    cat > "${cfg}" <<EOF
{"ports":[{"listen":":${PORT}","command":"sleep ${ss}","monitor":{"enabled":true,"interval_seconds":1},"idle_minutes":${idle}}]}
EOF

    local start=$(date +%s)

    # Start gatekeeper in background
    "${BIN}" "${cfg}" >"${EVIDENCE_DIR}/gk.txt" 2>&1 &
    local gkp=$!
    sleep 2  # let gatekeeper bind

    # Make a TCP connection to trigger backend startup
    # Use timeout to ensure we don't hang
    timeout 5 bash -c "echo 'GET / HTTP/1.0' | nc -w 2 127.0.0.1 ${PORT}" 2>/dev/null || true
    sleep 2  # let backend spawn

    # Verify backend (sleep) is running
    if ! pgrep -f "sleep ${ss}" >/dev/null 2>&1; then
        echo "FAIL: backend (sleep ${ss}) not spawned after connection" | tee -a "${LOG}"
        kill_all; rm -f "${cfg}"; return 1
    fi
    echo "  Backend spawned at $(date +%H:%M:%S)" | tee -a "${LOG}"

    # Wait for backend (sleep) to die — this is the idle timeout signal
    local timeout=$(( hi + 30 ))
    local dur
    while pgrep -f "sleep ${ss}" >/dev/null 2>&1; do
        sleep 1
        dur=$(( $(date +%s) - start ))
        if (( dur > timeout )); then
            echo "FAIL: timeout ${dur}s (expected ${lo}-${hi}s)" | tee -a "${LOG}"
            kill_all; rm -f "${cfg}"; return 1
        fi
    done
    dur=$(( $(date +%s) - start ))
    kill_all; rm -f "${cfg}"

    if (( dur >= lo && dur <= hi )); then
        echo "PASS: ${dur}s (expected ${lo}-${hi}s)" | tee -a "${LOG}"
    else
        echo "FAIL: ${dur}s (expected ${lo}-${hi}s)" | tee -a "${LOG}"
        return 1
    fi
}

all_ok=0
for v in 0 1 2 3; do
    run_test "${v}" || all_ok=1
done
exit ${all_ok}
