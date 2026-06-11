#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/.pio/build/qemu-esp32"
PIO="${ROOT_DIR}/venv/bin/pio"
ESPTOOL="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"
FLASH_IMAGE="${BUILD_DIR}/sertun32-qemu-4m.bin"

"${PIO}" run -e qemu-esp32

python3 "${ESPTOOL}" --chip esp32 merge_bin --fill-flash-size 4MB \
    -o "${FLASH_IMAGE}" \
    0x1000 "${BUILD_DIR}/bootloader.bin" \
    0x8000 "${BUILD_DIR}/partitions.bin" \
    0x10000 "${BUILD_DIR}/firmware.bin"

echo "QEMU flash image: ${FLASH_IMAGE}"
