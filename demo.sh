#!/bin/bash
set -euo pipefail

PORT="${PORT:-9443}"
DB="demo.db"
CERT_DIR=".data/certs"

# ---- Build if needed ----
if [ ! -f build/shift-js ] || [ ! -f build/sjsctl ]; then
    echo "Building..."
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
fi

# ---- Fresh database ----
rm -f "$DB" "$DB-wal" "$DB-shm" "$DB".logs_*.db "$DB".logs_*.db-wal "$DB".logs_*.db-shm "$DB".replay_*.log
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
