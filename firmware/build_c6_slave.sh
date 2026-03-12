#!/usr/bin/env bash
# Build the esp-hosted slave firmware for ESP32-C6 (SDIO transport).
# The resulting binary must be flashed to the C6 co-processor at address 0x0.
#
# Output: managed_components/espressif__esp_hosted/slave/build/network_adapter.bin
#
# Flash command (requires USB-to-TTL adapter on C6 UART test points):
#   esptool.py -p /dev/ttyUSB1 --chip esp32c6 --baud 460800 \
#     write_flash 0x0 managed_components/espressif__esp_hosted/slave/build/network_adapter.bin
set -e

SLAVE_DIR="managed_components/espressif__esp_hosted/slave"

echo "=== Building esp-hosted C6 slave firmware ==="

# Use the same container image, but override working dir and target
podman run --rm \
    -v "$(cd .. && pwd):/workspace" \
    -w "/workspace/concept/${SLAVE_DIR}" \
    "$(podman images --format '{{.Repository}}:{{.Tag}}' | grep 'localhost/concept' | head -1)" \
    set-target esp32c6

podman run --rm \
    -v "$(cd .. && pwd):/workspace" \
    -w "/workspace/concept/${SLAVE_DIR}" \
    "$(podman images --format '{{.Repository}}:{{.Tag}}' | grep 'localhost/concept' | head -1)" \
    build

echo ""
echo "=== Slave firmware built ==="
echo "Binary: ${SLAVE_DIR}/build/network_adapter.bin"
echo ""
echo "Flash to C6 (with USB-to-TTL on C6 UART test points, C6_IO9 pulled LOW at power-on):"
echo "  esptool.py -p /dev/ttyUSB1 --chip esp32c6 --baud 460800 \\"
echo "    write_flash 0x0 ${SLAVE_DIR}/build/network_adapter.bin"
