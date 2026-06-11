#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FLASH_IMAGE="${ROOT_DIR}/.pio/build/qemu-esp32/sertun32-qemu-4m.bin"
QEMU="${QEMU_ESP32:-qemu-system-xtensa}"
STATUS_LOG="${ROOT_DIR}/.pio/build/qemu-esp32/uart1-status.log"

if [[ ! -f "${FLASH_IMAGE}" ]]; then
    "${ROOT_DIR}/scripts/build_qemu.sh"
fi

if ! command -v "${QEMU}" >/dev/null 2>&1; then
    echo "Espressif qemu-system-xtensa was not found." >&2
    echo "Set QEMU_ESP32=/path/to/espressif/qemu-system-xtensa." >&2
    exit 1
fi

if ! "${QEMU}" -machine help 2>&1 | grep -qE '^esp32[[:space:]]'; then
    echo "${QEMU} does not provide the Espressif esp32 machine." >&2
    echo "Install Espressif's QEMU fork and set QEMU_ESP32 to its binary." >&2
    exit 1
fi

echo "UART0 data endpoint: tcp://127.0.0.1:30121"
echo "UART2 data endpoint: tcp://127.0.0.1:30122"
echo "Status log: ${STATUS_LOG}"
echo "Use the QEMU monitor command 'quit' to stop."

exec "${QEMU}" \
    -machine esp32 \
    -drive "file=${FLASH_IMAGE},if=mtd,format=raw" \
    -global driver=timer.esp32.timg,property=wdt_disable,value=true \
    -display none \
    -monitor stdio \
    -serial tcp:127.0.0.1:30121,server=on,wait=off \
    -serial "file:${STATUS_LOG}" \
    -serial tcp:127.0.0.1:30122,server=on,wait=off
