#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENVIRONMENT="${1:?usage: build_firmware.sh ENVIRONMENT [OUTPUT_DIR]}"
OUTPUT_DIR="${2:-${ROOT_DIR}/dist/${ENVIRONMENT}}"
PIO="${PIO:-${ROOT_DIR}/venv/bin/pio}"

if [[ ! -x "${PIO}" ]]; then
    PIO="$(command -v pio)"
fi

export LC_ALL=C
export TZ=UTC
export PYTHONHASHSEED=0
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${ROOT_DIR}" log -1 --format=%ct)}"

if [[ "${ENVIRONMENT}" == "qemu-esp32" ]]; then
    PIO="${PIO}" "${ROOT_DIR}/scripts/build_qemu.sh"
else
    "${PIO}" run --project-dir "${ROOT_DIR}" -e "${ENVIRONMENT}"
fi

BUILD_DIR="${ROOT_DIR}/.pio/build/${ENVIRONMENT}"
mkdir -p "${OUTPUT_DIR}"

for artifact in firmware.bin firmware.elf bootloader.bin partitions.bin; do
    install -m 0644 "${BUILD_DIR}/${artifact}" "${OUTPUT_DIR}/${artifact}"
done

if [[ -f "${BUILD_DIR}/sertun32-qemu-4m.bin" ]]; then
    install -m 0644 \
        "${BUILD_DIR}/sertun32-qemu-4m.bin" \
        "${OUTPUT_DIR}/sertun32-qemu-4m.bin"
fi

(
    cd "${OUTPUT_DIR}"
    sha256sum ./*.bin ./*.elf | LC_ALL=C sort -k2 > SHA256SUMS
)
