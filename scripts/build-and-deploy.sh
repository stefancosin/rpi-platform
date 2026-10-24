#!/usr/bin/env bash
# Cross-compiles rpi-platform for the RPi (aarch64) on this desktop, ships
# the binary over, and restarts the service. Overwriting the binary in
# place fails while the old process is running (ETXTBSY) - scp to a temp
# name and rename into place instead, which works even while it's busy.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-aarch64"
RPI_HOST="stefanPI"
RPI_BUILD_DIR="/home/stefan/Projects/rpi-platform/build"

echo "==> Configuring (aarch64 cross toolchain)"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${PROJECT_DIR}/cmake/toolchain-aarch64.cmake"

echo "==> Building"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "==> Uploading binary"
scp "${BUILD_DIR}/rpi-platform" "${RPI_HOST}:${RPI_BUILD_DIR}/rpi-platform.new"

echo "==> Installing + restarting service on ${RPI_HOST}"
ssh "${RPI_HOST}" "
    chmod +x '${RPI_BUILD_DIR}/rpi-platform.new' &&
    mv '${RPI_BUILD_DIR}/rpi-platform.new' '${RPI_BUILD_DIR}/rpi-platform' &&
    sudo systemctl restart rpi-platform.service &&
    sleep 1 &&
    sudo systemctl status rpi-platform.service --no-pager -l
"

echo "==> Done"
