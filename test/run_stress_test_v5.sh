#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}/.."
GATEKEEPER="${PROJECT_ROOT}/build/linux/x86_64/release/gatekeeper"
CONFIG_TEMPLATE="${PROJECT_ROOT}/gatekeeper-config.json"
TMP_CONFIG="/tmp/gatekeeper_test_config.json"
EVIDENCE="${PROJECT_ROOT}/.omo/evidence/memory-leak-detection/task-3-simple-stress.txt"
ASAN_LOG="/tmp/asan_output.log"

# Port for gatekeeper simple mode
GATEKEEPER_PORT=19999

# Prepare config JSON: set listen port and command to start a simple Python HTTP server on same port
jq ".ports[0].listen = \":$GATEKEEPER_PORT\" | .ports[0].command = \"python3 -m http.server $GATEKEEPER_PORT\"" "$CONFIG_TEMPLATE" > "$TMP_CONFIG"

export LSAN_OPTIONS="exitcode=0:detect_leaks=1"

# Run gatekeeper with timeout (30s) and capture stderr
timeout 30s "$GATEKEEPER" "$TMP_CONFIG" 2> "$ASAN_LOG" &
GK_PID=$!
# Give it a moment to start and launch backend
sleep 3

# Record RSS before loop
RSS_BEFORE=$(ps -o rss= -p $GK_PID || echo 0)

# Perform 1000 connection cycles (or until gatekeeper exits)
for i in $(seq 1 1000); do
    if ! kill -0 $GK_PID 2>/dev/null; then break; fi
    echo -n "" | nc -w 1 127.0.0.1 $GATEKEEPER_PORT || true
done

# Record RSS after loop
RSS_AFTER=$(ps -o rss= -p $GK_PID || echo 0)

# Cleanup
kill $GK_PID 2>/dev/null || true
wait $GK_PID 2>/dev/null || true

# Count leak reports
LEAK_COUNT=$(grep -c "LeakSanitizer" "$ASAN_LOG" || true)

# Write evidence summary
{
    echo "=== Stress Test Summary ==="
    echo "Gatekeeper port: $GATEKEEPER_PORT"
    echo "RSS before (KB): $RSS_BEFORE"
    echo "RSS after (KB): $RSS_AFTER"
    echo "RSS delta (KB): $((RSS_AFTER - RSS_BEFORE))"
    echo "Leak reports found: $LEAK_COUNT"
    echo "--- ASAN Log excerpt (first 20 lines) ---"
    head -n 20 "$ASAN_LOG"
} > "$EVIDENCE"

cat "$EVIDENCE"
