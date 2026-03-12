#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"
PORT="${ESPPORT:-/dev/ttyUSB0}"

podman run --rm -it \
  --privileged \
  -v "${WORKSPACE}:/workspace" \
  -w /workspace/concept \
  -e IDF_TARGET=esp32p4 \
  -e ESPPORT="${PORT}" \
  --device "${PORT}:${PORT}" \
  localhost/concept_idf:latest \
  flash monitor "$@"
