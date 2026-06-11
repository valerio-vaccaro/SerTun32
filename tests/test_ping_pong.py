"""Strict one-byte USB <-> TCP round-trip benchmark."""

import hashlib
import time

import pytest

from conftest import LOGGER, receive_exact


@pytest.mark.pingpong
def test_one_byte_ping_pong(tunnel, request):
    count = request.config.getoption("--ping-pong-count")
    log_every = request.config.getoption("--ping-pong-log-every")
    assert count > 0
    assert log_every > 0

    serial_port = tunnel["serial"]
    connection = tunnel["socket"]
    sent_hash = hashlib.sha256()
    returned_hash = hashlib.sha256()

    LOGGER.info(
        "PINGPONG start iterations=%s payload_bytes_per_iteration=1 "
        "strict_round_trip=1",
        count,
    )
    started = time.monotonic()

    for iteration in range(count):
        payload = bytes((iteration & 0xFF,))
        written = serial_port.write(payload)
        assert written == 1, f"iteration {iteration}: serial wrote {written} bytes"

        tcp_byte = receive_exact(connection.recv, 1)
        assert tcp_byte == payload, (
            f"iteration {iteration}: USB->TCP byte {tcp_byte.hex()} "
            f"!= {payload.hex()}"
        )

        previous_guard, next_guard = tunnel["rotate_usb_tx_guard"](
            log_rotation=False
        )
        connection.sendall(payload)
        connection.sendall(next_guard)
        usb_bytes = receive_exact(serial_port.read, len(previous_guard) + 1)
        assert usb_bytes == previous_guard + payload, (
            f"iteration {iteration}: TCP->USB response mismatch"
        )

        sent_hash.update(payload)
        returned_hash.update(usb_bytes[-1:])

        completed = iteration + 1
        if completed % log_every == 0 or completed == count:
            elapsed = time.monotonic() - started
            LOGGER.info(
                "PINGPONG progress completed=%s total=%s percent=%.3f "
                "elapsed=%.6fs round_trips_per_second=%.3f",
                completed,
                count,
                completed * 100.0 / count,
                elapsed,
                completed / elapsed if elapsed else 0,
            )

    elapsed = time.monotonic() - started
    sent_digest = sent_hash.hexdigest()
    returned_digest = returned_hash.hexdigest()
    LOGGER.info(
        "PINGPONG finish iterations=%s elapsed=%.6fs "
        "average_round_trip_us=%.3f round_trips_per_second=%.3f "
        "payload_bytes_each_direction=%s sent_sha256=%s returned_sha256=%s",
        count,
        elapsed,
        elapsed * 1_000_000 / count,
        count / elapsed if elapsed else 0,
        count,
        sent_digest,
        returned_digest,
    )
    assert returned_digest == sent_digest
