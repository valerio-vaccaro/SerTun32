"""Timed throughput and application-write burst tests for SerTun32."""

import hashlib
import random
import threading
import time

import pytest

from conftest import LOGGER


def deterministic_bytes(size, seed):
    return random.Random(seed).randbytes(size)


TIMED_VECTORS = [
    pytest.param(deterministic_bytes(64, 64), id="small-random-64B"),
    pytest.param(b"SerTun32-throughput-" * 16384, id="repeated-320KiB"),
    pytest.param(
        deterministic_bytes(1024 * 1024, 0xB16B00B5),
        id="large-random-1MiB",
        marks=pytest.mark.large_payload,
    ),
]

BURST_CASES = [
    pytest.param(10, 1024, id="10-packets"),
    pytest.param(50, 1024, id="50-packets"),
    pytest.param(100, 1024, id="100-packets"),
]

LARGE_MESSAGE_COUNT = 3
LARGE_MESSAGE_SIZE = 1024 * 1024


def receive_exact(reader, size):
    data = bytearray()
    while len(data) < size:
        chunk = reader(size - len(data))
        if not chunk:
            raise RuntimeError(f"received {len(data)} of {size} bytes")
        data.extend(chunk)
    return bytes(data)


def run_timed_transfer(packets, writer, reader, expected=None):
    wire_payload = b"".join(packets)
    if expected is None:
        expected = wire_payload
    errors = []

    def send_packets():
        try:
            for packet in packets:
                writer(packet)
        except Exception as exc:
            errors.append(exc)

    started = time.monotonic()
    sender = threading.Thread(target=send_packets, daemon=True)
    sender.start()
    received = receive_exact(reader, len(expected))
    sender.join(timeout=60)
    elapsed = time.monotonic() - started

    if sender.is_alive():
        raise TimeoutError("sender did not finish within 60 seconds")
    if errors:
        raise errors[0]

    return expected, received, elapsed


def report_metrics(direction, case, byte_count, packet_count, elapsed):
    throughput = byte_count / elapsed / 1024 if elapsed else 0
    packet_rate = packet_count / elapsed if elapsed else 0
    LOGGER.info(
        "PERF direction=%s case=%s time=%.6fs bytes=%s packets=%s "
        "rate=%.3f_KiB/s packets_per_second=%.3f",
        direction,
        case,
        elapsed,
        byte_count,
        packet_count,
        throughput,
        packet_rate,
    )


def assert_integrity(expected, received, direction):
    expected_hash = hashlib.sha256(expected).hexdigest()
    received_hash = hashlib.sha256(received).hexdigest()
    LOGGER.info(
        "INTEGRITY direction=%s expected_bytes=%s received_bytes=%s "
        "expected_sha256=%s received_sha256=%s",
        direction,
        len(expected),
        len(received),
        expected_hash,
        received_hash,
    )
    assert received_hash == expected_hash, f"{direction} payload SHA-256 mismatch"


@pytest.mark.performance
@pytest.mark.parametrize("payload", TIMED_VECTORS)
def test_timed_usb_to_tcp(tunnel, payload, request):
    expected, received, elapsed = run_timed_transfer(
        [payload],
        tunnel["serial"].write,
        lambda size: tunnel["socket"].recv(min(size, 65536)),
    )
    assert_integrity(expected, received, "USB->TCP")
    report_metrics("USB->TCP", request.node.callspec.id, len(payload), 1, elapsed)
    assert elapsed < 60


@pytest.mark.performance
@pytest.mark.parametrize("payload", TIMED_VECTORS)
def test_timed_tcp_to_usb(tunnel, payload, request):
    previous_guard, next_guard = tunnel["rotate_usb_tx_guard"]()
    expected, received, elapsed = run_timed_transfer(
        [payload, next_guard],
        tunnel["socket"].sendall,
        tunnel["serial"].read,
        expected=previous_guard + payload,
    )
    assert_integrity(expected, received, "TCP->USB")
    report_metrics("TCP->USB", request.node.callspec.id, len(payload), 1, elapsed)
    assert elapsed < 60


def make_burst(packet_count, packet_size):
    packets = []
    for sequence in range(packet_count):
        header = sequence.to_bytes(4, "little")
        body = deterministic_bytes(packet_size - len(header), 0x30121 + sequence)
        packets.append(header + body)
    return packets


@pytest.mark.performance
@pytest.mark.parametrize(("packet_count", "packet_size"), BURST_CASES)
def test_burst_usb_to_tcp(tunnel, packet_count, packet_size, request):
    packets = make_burst(packet_count, packet_size)
    expected, received, elapsed = run_timed_transfer(
        packets,
        tunnel["serial"].write,
        lambda size: tunnel["socket"].recv(min(size, 65536)),
    )
    assert_integrity(expected, received, "USB->TCP")
    report_metrics(
        "USB->TCP", request.node.callspec.id, len(expected), packet_count, elapsed
    )
    assert elapsed < 60


@pytest.mark.performance
@pytest.mark.parametrize(("packet_count", "packet_size"), BURST_CASES)
def test_burst_tcp_to_usb(tunnel, packet_count, packet_size, request):
    previous_guard, next_guard = tunnel["rotate_usb_tx_guard"]()
    packets = make_burst(packet_count, packet_size)
    expected, received, elapsed = run_timed_transfer(
        packets + [next_guard],
        tunnel["socket"].sendall,
        tunnel["serial"].read,
        expected=previous_guard + b"".join(packets),
    )
    assert_integrity(expected, received, "TCP->USB")
    report_metrics(
        "TCP->USB",
        request.node.callspec.id,
        packet_count * packet_size,
        packet_count,
        elapsed,
    )
    assert elapsed < 60


@pytest.mark.performance
@pytest.mark.large_transfer
def test_three_1mib_messages_both_directions(tunnel):
    direction_elapsed = {"USB->TCP": 0.0, "TCP->USB": 0.0}

    for sequence in range(LARGE_MESSAGE_COUNT):
        payload = deterministic_bytes(LARGE_MESSAGE_SIZE, 0x10_0000 + sequence)
        expected, received, elapsed = run_timed_transfer(
            [payload],
            tunnel["serial"].write,
            lambda size: tunnel["socket"].recv(min(size, 65536)),
        )
        assert_integrity(expected, received, f"USB->TCP message={sequence + 1}")
        direction_elapsed["USB->TCP"] += elapsed
        LOGGER.info(
            "LARGE_TRANSFER direction=USB->TCP message=%s/%s "
            "bytes=%s time=%.6fs",
            sequence + 1,
            LARGE_MESSAGE_COUNT,
            LARGE_MESSAGE_SIZE,
            elapsed,
        )

    for sequence in range(LARGE_MESSAGE_COUNT):
        payload = deterministic_bytes(LARGE_MESSAGE_SIZE, 0x20_0000 + sequence)
        previous_guard, next_guard = tunnel["rotate_usb_tx_guard"](
            log_rotation=False
        )
        expected, received, elapsed = run_timed_transfer(
            [payload, next_guard],
            tunnel["socket"].sendall,
            tunnel["serial"].read,
            expected=previous_guard + payload,
        )
        assert_integrity(expected, received, f"TCP->USB message={sequence + 1}")
        direction_elapsed["TCP->USB"] += elapsed
        LOGGER.info(
            "LARGE_TRANSFER direction=TCP->USB message=%s/%s "
            "bytes=%s time=%.6fs",
            sequence + 1,
            LARGE_MESSAGE_COUNT,
            LARGE_MESSAGE_SIZE,
            elapsed,
        )

    total_bytes = LARGE_MESSAGE_COUNT * LARGE_MESSAGE_SIZE
    for direction, elapsed in direction_elapsed.items():
        report_metrics(
            direction,
            "3-messages-1MiB",
            total_bytes,
            LARGE_MESSAGE_COUNT,
            elapsed,
        )
        assert elapsed < LARGE_MESSAGE_COUNT * 60
