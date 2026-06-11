"""Parametrized binary-integrity tests for the USB <-> TCP tunnel."""

import hashlib
import random
import threading
import time

import pytest

from conftest import LOGGER


def deterministic_bytes(size, seed):
    return random.Random(seed).randbytes(size)


PAYLOADS = [
    pytest.param(b"", id="empty"),
    pytest.param(b"A", id="one-byte"),
    pytest.param(b"SerTun32 v1.2.0\r\n", id="ascii"),
    pytest.param("Caffè € Bitcoin ₿".encode("utf-8"), id="utf8"),
    pytest.param("SerTun32 日本語".encode("utf-16-le"), id="utf16-le"),
    pytest.param("Tunnel 🔒".encode("utf-32-le"), id="utf32-le"),
    pytest.param(bytes(range(256)), id="all-byte-values"),
    pytest.param(b"\x00\xff\x11\x13" * 2048, id="control-bytes-8K"),
    pytest.param(deterministic_bytes(1023, 1023), id="random-1023"),
    pytest.param(deterministic_bytes(1024, 1024), id="random-1K"),
    pytest.param(deterministic_bytes(4097, 4097), id="random-4K-plus-1"),
    pytest.param(deterministic_bytes(65537, 65537), id="random-64K-plus-1"),
    pytest.param(
        deterministic_bytes(1024 * 1024, 0x5E7A32),
        id="random-1MiB",
        marks=pytest.mark.large_payload,
    ),
]


def receive_exact(reader, size):
    data = bytearray()
    while len(data) < size:
        chunk = reader(size - len(data))
        if not chunk:
            raise RuntimeError(f"received {len(data)} of {size} bytes")
        data.extend(chunk)
    return bytes(data)


def transfer(payload, writer, reader):
    if not payload:
        return b""

    errors = []

    def send():
        try:
            writer(payload)
        except Exception as exc:
            errors.append(exc)

    sender = threading.Thread(target=send, daemon=True)
    sender.start()
    received = receive_exact(reader, len(payload))
    sender.join(timeout=30)

    if sender.is_alive():
        raise TimeoutError("sender did not finish within 30 seconds")
    if errors:
        raise errors[0]
    return received


def assert_payload_equal(payload, received, direction):
    expected_hash = hashlib.sha256(payload).hexdigest()
    received_hash = hashlib.sha256(received).hexdigest()
    LOGGER.info(
        "INTEGRITY direction=%s expected_bytes=%s received_bytes=%s "
        "expected_sha256=%s received_sha256=%s",
        direction,
        len(payload),
        len(received),
        expected_hash,
        received_hash,
    )
    assert received_hash == expected_hash, (
        f"{direction} SHA-256 mismatch: {received_hash} != {expected_hash}"
    )


@pytest.mark.parametrize("payload", PAYLOADS)
def test_usb_to_tcp(tunnel, payload):
    started = time.monotonic()
    received = transfer(
        payload,
        tunnel["serial"].write,
        lambda size: tunnel["socket"].recv(min(size, 65536)),
    )
    assert_payload_equal(payload, received, "USB->TCP")
    elapsed = time.monotonic() - started
    LOGGER.info(
        "TRANSFER direction=USB->TCP bytes=%s time=%.6fs rate=%.3f_KiB/s",
        len(payload),
        elapsed,
        len(payload) / elapsed / 1024 if elapsed else 0,
    )
    assert elapsed < 30


@pytest.mark.parametrize("payload", PAYLOADS)
def test_tcp_to_usb(tunnel, payload):
    previous_guard, next_guard = tunnel["rotate_usb_tx_guard"]()
    wire_payload = payload + next_guard
    started = time.monotonic()
    received = transfer(
        wire_payload,
        tunnel["socket"].sendall,
        tunnel["serial"].read,
    )
    assert_payload_equal(previous_guard + payload, received, "TCP->USB")
    elapsed = time.monotonic() - started
    LOGGER.info(
        "TRANSFER direction=TCP->USB payload_bytes=%s wire_guard_bytes=64 "
        "time=%.6fs rate=%.3f_KiB/s",
        len(payload),
        elapsed,
        len(payload) / elapsed / 1024 if elapsed else 0,
    )
    assert elapsed < 30
