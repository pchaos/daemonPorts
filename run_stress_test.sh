#!/usr/bin/env bash
set -euo pipefail

# Paths
GATEKEEPER="/home/user/myDocs/YUNIO/tmp/gupiao/daemonPorts/build/linux/x86_64/release/gatekeeper"
CONFIG_TEMPLATE="/home/user/myDocs/YUNIO/tmp/gupiao/daemonPorts/gatekeeper-config.json"
TMP_CONFIG="/tmp/gatekeeper_test_config.json"
EVIDENCE="/home/user/myDocs/YUNIO/tmp/gupiao/daemonPorts/.omo/evidence/memory-leak-detection/task-3-simple-stress.txt"
ASAN_LOG="/tmp/asan_output.log"

# Choose random backend port
BACKEND_PORT=$(shuf -i 20000-30000 -n 1)
GATEKEEPER_PORT=19999

# Create simple backend (Python HTTP server) in background
python3 - <<PY &
import http.server, socketserver, sys
class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"OK")

with socketserver.TCPServer(("127.0.0.1", $BACKEND_PORT), Handler) as httpd:
    httpd.serve_forever()
PY
BACKEND_PID=$!

# Prepare config JSON (modify backend address and listen port)
jq ".listen_port = \"$GATEKEEPER_PORT\" | .backend = \"127.0.0.1:$BACKEND_PORT\"" "$CONFIG_TEMPLATE" > "$TMP_CONFIG"

# Start gatekeeper with ASan options, redirect stderr to log
export LSAN_OPTIONS="exitcode=0:detect_leaks=1"
"$GATEKEEPER" -c "$TMP_CONFIG" 2> "$ASAN_LOG" &
GK_PID=$!
# Give it a moment to start
sleep 2

# Record RSS before
RSS_BEFORE=$(ps -o rss= -p $GK_PID)

# Perform 1000 connection cycles
for i in $(seq 1 1000); do
    # Connect via netcat, send nothing, close
    echo -n "" | nc -w 1 127.0.0.1 $GATEKEEPER_PORT || true
done

# Record RSS after
RSS_AFTER=$(ps -o rss= -p $GK_PID)

# Kill processes
kill $GK_PID $BACKEND_PID || true
wait $GK_PID $BACKEND_PID 2>/dev/null || true

# Check for leak reports in ASAN log
LEAK_COUNT=$(grep -c "LeakSanitizer" "$ASAN_LOG" || true)

# Write evidence summary
{
    echo "=== Stress Test Summary ==="
    echo "Backend port: $BACKEND_PORT"
    echo "Gatekeeper port: $GATEKEEPER_PORT"
    echo "RSS before (KB): $RSS_BEFORE"
    echo "RSS after (KB): $RSS_AFTER"
    echo "RSS delta (KB): $((RSS_AFTER - RSS_BEFORE))"
    echo "Leak reports found: $LEAK_COUNT"
    echo "--- ASAN Log excerpt (first 20 lines) ---"
    head -n 20 "$ASAN_LOG"
} > "$EVIDENCE"

# Also output evidence to stdout for logging
cat "$EVIDENCE"
