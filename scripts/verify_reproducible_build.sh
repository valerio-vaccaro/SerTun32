#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENVIRONMENT="${1:?usage: verify_reproducible_build.sh ENVIRONMENT}"
FIRST_DIR="${ROOT_DIR}/dist/reproducibility/${ENVIRONMENT}/first"
SECOND_DIR="${ROOT_DIR}/dist/reproducibility/${ENVIRONMENT}/second"
FINAL_DIR="${ROOT_DIR}/dist/${ENVIRONMENT}"
SECOND_ROOT="$(mktemp -d "/tmp/sertun32-repro-${ENVIRONMENT}.XXXXXX")"

cleanup() {
    rm -rf "${SECOND_ROOT}"
}
trap cleanup EXIT

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${ROOT_DIR}" log -1 --format=%ct)}"

rm -rf "${ROOT_DIR}/.pio/build" \
    "${ROOT_DIR}/dist/reproducibility/${ENVIRONMENT}" \
    "${FINAL_DIR}"
"${ROOT_DIR}/scripts/build_firmware.sh" "${ENVIRONMENT}" "${FIRST_DIR}"

tar \
    --exclude=./.git \
    --exclude=./.pio \
    --exclude=./dist \
    --exclude=./venv \
    -C "${ROOT_DIR}" -cf - . |
    tar -C "${SECOND_ROOT}" -xf -

PIO="${PIO:-${ROOT_DIR}/venv/bin/pio}" \
    "${SECOND_ROOT}/scripts/build_firmware.sh" \
    "${ENVIRONMENT}" \
    "${SECOND_ROOT}/dist/${ENVIRONMENT}"
mkdir -p "${SECOND_DIR}"
cp -a "${SECOND_ROOT}/dist/${ENVIRONMENT}/." "${SECOND_DIR}/"

diff -u "${FIRST_DIR}/SHA256SUMS" "${SECOND_DIR}/SHA256SUMS"

mkdir -p "${FINAL_DIR}"
cp -a "${FIRST_DIR}/." "${FINAL_DIR}/"
echo "Reproducible build verified for ${ENVIRONMENT}"
