#!/bin/bash
set -euo pipefail

PORT="${PORT:-9443}"
DB="demo.db"
CERT_DIR=".data/certs"

# ---- Kill stray shift-js processes ----
if pgrep -f "shift-js" >/dev/null 2>&1; then
    echo "Killing stray shift-js processes..."
    pkill -9 -f "shift-js" 2>/dev/null || true
    sleep 1
fi

# ---- Build if needed ----
if [ ! -f build/shift-js ] || [ ! -f build/sjsctl ]; then
    echo "Building..."
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
fi

# ---- Fresh database ----
rm -f "$DB" "$DB-wal" "$DB-shm" logs.db logs.db-wal logs.db-shm
echo "Uploading demo files..."
./build/sjsctl -d "$DB" upload demo

# ---- TLS certs via mkcert ----
mkdir -p "$CERT_DIR"
mkcert -install 2>/dev/null || true
mkcert -cert-file "$CERT_DIR/cert.pem" -key-file "$CERT_DIR/key.pem" \
    localhost 127.0.0.1 ::1 2>/dev/null
echo "Generated TLS certs for localhost"

./build/sjsctl -d "$DB" cert-put default "$CERT_DIR/cert.pem" "$CERT_DIR/key.pem"

# ---- Start server ----
echo ""
echo "Starting shift-js on https://localhost:$PORT"
echo "Press Ctrl-C to stop"
echo ""
exec ./build/shift-js -d "$DB" -p "$PORT" -w 1 -t
