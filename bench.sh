#!/bin/bash
set -euo pipefail

DB="${DB:-/tmp/shift-js-bench.db}"
PORT="${PORT:-9000}"
BINARY="${BINARY:-build-prof/shift-js}"
SJSCTL="${SJSCTL:-build-prof/sjsctl}"
DURATION="${DURATION:-10}"

# --- Setup ---
pkill -9 -f "shift-js.*-p $PORT" 2>/dev/null || true
sleep 1

# Seed test modules
rm -f "$DB"
"$SJSCTL" -d "$DB" put "__code/index.mjs" \
    'export function get() { return { status: "ok", path: request.path }; }'
"$SJSCTL" -d "$DB" put "__code/hello/index.mjs" \
    'export function get() { return "hello world"; }'

echo "=== shift-js load test ==="
echo "binary: $BINARY"
echo "db:     $DB"
echo "port:   $PORT"
echo "duration: ${DURATION}s per test"
echo ""

run_test() {
    local label="$1" workers="$2" clients="$3" streams="$4" path="$5"

    pkill -9 -f "shift-js.*-p $PORT" 2>/dev/null || true
    sleep 1

    "$BINARY" -d "$DB" -p "$PORT" -w "$workers" 2>/dev/null &
    local pid=$!
    sleep 2

    # Warm up
    nghttp "http://localhost:$PORT$path" 2>/dev/null > /dev/null
    nghttp "http://localhost:$PORT$path" 2>/dev/null > /dev/null

    echo "--- $label (${workers}w, ${clients}c, ${streams}m) ---"
    h2load -c"$clients" -m"$streams" -D"$DURATION" "http://localhost:$PORT$path" 2>&1 \
        | grep -E "finished|requests:|time for req|req/s"
    echo ""

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# Serial latency (single connection, single stream)
run_test "serial /hello"        1   1   1   /hello
run_test "serial / (JSON)"      1   1   1   /

# Single worker throughput
run_test "1w throughput /hello"  1  10 100   /hello
run_test "1w throughput / JSON"  1  10 100   /

# Scaling tests
for w in 2 4 8; do
    c=$((w * 10))
    run_test "${w}w throughput /hello" "$w" "$c" 100 /hello
done

echo "=== done ==="
