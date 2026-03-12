#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"

podman run --rm \
  -v "${WORKSPACE}:/workspace:z" \
  -w /workspace/concept \
  -e IDF_TARGET=esp32p4 \
  localhost/concept_idf:latest \
  build "$@"
