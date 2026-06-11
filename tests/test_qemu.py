"""Firmware-in-the-loop tests for the ESP32 QEMU UART bridge."""

import hashlib
import random

import pytest

from conftest import LOGGER, receive_exact


def deterministic_bytes(size, seed):
    return random.Random(seed).randbytes(size)


PAYLOADS = [
    pytest.param(b"A", id="one-byte"),
    pytest.param(bytes(range(256)), id="all-byte-values"),
    pytest.param(b"\x00\xff\x11\x13" * 2048, id="control-bytes-8K"),
    pytest.param(deterministic_bytes(65537, 0x5E7A32), id="random-64K-plus-1"),
]


def assert_transfer(source, destination, payload, direction):
    source.sendall(payload)
    received = receive_exact(destination.recv, len(payload))
    expected_hash = hashlib.sha256(payload).hexdigest()
    received_hash = hashlib.sha256(received).hexdigest()
    LOGGER.info(
        "QEMU direction=%s bytes=%s expected_sha256=%s received_sha256=%s",
        direction,
        len(payload),
        expected_hash,
        received_hash,
    )
    assert received == payload


@pytest.mark.qemu
@pytest.mark.parametrize("payload", PAYLOADS)
def test_uart0_to_uart2(qemu_uart_bridge, payload):
    assert_transfer(
        qemu_uart_bridge["uart0"],
        qemu_uart_bridge["uart2"],
        payload,
        "UART0->UART2",
    )


@pytest.mark.qemu
@pytest.mark.parametrize("payload", PAYLOADS)
def test_uart2_to_uart0(qemu_uart_bridge, payload):
    assert_transfer(
        qemu_uart_bridge["uart2"],
        qemu_uart_bridge["uart0"],
        payload,
        "UART2->UART0",
    )
