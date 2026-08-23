#!/usr/bin/env bash
# Integration test: launch_on_start — verify backend is launched immediately
# at gatekeeper startup (no connection needed), and NOT launched when flag absent.
set -euo pipefail

EVIDENCE_DIR=".omo/evidence/launch-on-start-integration-test"
mkdir -p "${EVIDENCE_DIR}"
LOG="${EVIDENCE_DIR}/test-run.log"
: > "${LOG}"

BIN="./build/linux/x86_64/debug/gatekeeper"
PORT_A=37810  # launch_on_start=true
PORT_B=37811  # launch_on_start=false (control group)

kill_all() {
    pkill -9 -f gatekeeper 2>/dev/null || true
    pkill -9 -f "sleep 60" 2>/dev/null || true
}

fail() {
    echo "FAIL: $1" | tee -a "${LOG}"
    kill_all; return 1
}

# ── Case 1: launch_on_start=true → backend running without any connection ──
echo "=== Case 1: launch_on_start=true (auto launch at startup) ===" | tee -a "${LOG}"
kill_all; sleep 1
cfg=$(mktemp)
cat > "${cfg}" <<EOF
{"ports":[{"listen":":${PORT_A}","command":"sleep 60","launch_on_start":true}]}
EOF

"${BIN}" "${cfg}" >"${EVIDENCE_DIR}/gk-case1.txt" 2>&1 &
local_gkp=$!
sleep 2  # let gatekeeper bind and launch backend

if pgrep -f "sleep 60" >/dev/null 2>&1; then
    echo "PASS: backend (sleep 60) running immediately after startup" | tee -a "${LOG}"
else
    echo "--- gatekeeper log ---" | tee -a "${LOG}"
    cat "${EVIDENCE_DIR}/gk-case1.txt" | tee -a "${LOG}"
    kill_all; rm -f "${cfg}"; exit 1
fi

rm -f "${cfg}"

# ── Case 2: No launch_on_start → backend NOT running until first connection ──
echo "=== Case 2: default (launch_on_start absent) → backend waits for connection ===" | tee -a "${LOG}"
kill_all; sleep 1
cfg=$(mktemp)
cat > "${cfg}" <<EOF
{"ports":[{"listen":":${PORT_B}","command":"sleep 60"}]}
EOF

"${BIN}" "${cfg}" >"${EVIDENCE_DIR}/gk-case2.txt" 2>&1 &
local_gkp=$!
sleep 2  # let gatekeeper bind

if pgrep -f "sleep 60" >/dev/null 2>&1; then
    fail "backend launched without launch_on_start (flag should be off by default)"
fi
echo "PASS: backend NOT running before first connection" | tee -a "${LOG}"

# Now trigger a connection → backend should launch
timeout 3 bash -c "echo 'GET / HTTP/1.0' | nc -w 2 127.0.0.1 ${PORT_B}" >/dev/null 2>&1 || true
sleep 2
if pgrep -f "sleep 60" >/dev/null 2>&1; then
    echo "PASS: backend launched after first connection (existing behavior intact)" | tee -a "${LOG}"
else
    fail "backend did not launch after first connection"
fi

kill_all; rm -f "${cfg}"
echo "ALL PASS" | tee -a "${LOG}"