#!/bin/bash
# Flash ESP32-P4 using host esptool (no Podman TTY needed)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
PORT="${ESPPORT:-/dev/ttyUSB0}"

if [ ! -f "${BUILD_DIR}/flash_args" ]; then
    echo "ERROR: build/flash_args not found — run build first"
    exit 1
fi

echo "Flashing to ${PORT}..."
cd "${BUILD_DIR}"
python -m esptool --chip esp32p4 -p "${PORT}" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash "@flash_args"

echo "Done."
