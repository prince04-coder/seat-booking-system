#!/usr/bin/env bash
set -euo pipefail

echo "=== Ticketing Engine Setup ==="

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# Step 1: Start MySQL via Docker
echo -e "${GREEN}[1/3]${NC} Starting MySQL via Docker..."
if command -v docker &> /dev/null; then
    docker compose up -d 2>/dev/null || docker-compose up -d 2>/dev/null || {
        echo -e "${YELLOW}Warning:${NC} Docker Compose failed. Ensure MySQL is running on port 3306."
    }
    echo "Waiting for MySQL to be ready..."
    sleep 8
else
    echo -e "${YELLOW}[1/3]${NC} Docker not found. Ensure MySQL is running on localhost:3306."
fi

# Step 2: Apply schema (also auto-applied via Docker init volume)
echo -e "${GREEN}[2/3]${NC} Applying database schema..."
mysql -h 127.0.0.1 -u root -proot ticketing < sql/schema.sql 2>/dev/null && \
    echo "Schema applied." || \
    echo -e "${YELLOW}Note:${NC} Schema may already be applied via Docker init — that's fine."

# Step 3: Build
echo -e "${GREEN}[3/3]${NC} Building..."
if [ -z "${VCPKG_ROOT:-}" ]; then
    echo -e "${RED}Error:${NC} VCPKG_ROOT is not set."
    echo "  export VCPKG_ROOT=/path/to/vcpkg"
    exit 1
fi

cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=Release

CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build build -j"${CPU_COUNT}"

echo ""
echo -e "${GREEN}Build complete!${NC}"
echo ""
echo "┌──────────────────────────────────────────────┐"
echo "│  To run the server:                          │"
echo "│    ./build/ticketing_engine                  │"
echo "│                                              │"
echo "│  Then open: http://localhost:18080            │"
echo "└──────────────────────────────────────────────┘"
