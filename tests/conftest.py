"""Hardware fixtures for end-to-end SerTun32 tunnel tests."""

import hashlib
import logging
from pathlib import Path
import platform
import socket
import struct
import sys
import time

import pytest
import serial

LOGGER = logging.getLogger("sertun32")


def receive_exact(reader, size):
    data = bytearray()
    while len(data) < size:
        chunk = reader(size - len(data))
        if not chunk:
            raise RuntimeError(f"received {len(data)} of {size} sync bytes")
        data.extend(chunk)
    return bytes(data)


def pytest_addoption(parser):
    group = parser.getgroup("sertun32")
    group.addoption("--serial-port", default="/dev/ttyACM0")
    group.addoption("--tcp-port", type=int, default=30121)
    group.addoption("--connect-timeout", type=float, default=30.0)
    group.addoption("--startup-delay", type=float, default=10.0)
    group.addoption(
        "--enable-large-tests",
        action="store_true",
        default=False,
        help="run 1 MiB payload cases and large-transfer stress tests",
    )
    group.addoption(
        "--sertun-log",
        default="test-results/sertun32-pytest.log",
        help="SerTun32 text log path",
    )
    group.addoption("--ping-pong-count", type=int, default=1_000)
    group.addoption("--ping-pong-log-every", type=int, default=100)
    group.addoption("--qemu-uart0-port", type=int, default=30121)
    group.addoption("--qemu-uart2-port", type=int, default=30122)
    group.addoption(
        "--qemu-status-log",
        default=".pio/build/qemu-esp32/uart1-status.log",
    )


def pytest_configure(config):
    log_path = Path(config.getoption("--sertun-log")).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)

    LOGGER.setLevel(logging.INFO)
    LOGGER.handlers.clear()
    formatter = logging.Formatter(
        "%(asctime)s.%(msecs)03d %(levelname)-5s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(formatter)
    LOGGER.addHandler(console)

    log_file = logging.FileHandler(log_path, mode="w", encoding="utf-8")
    log_file.setFormatter(formatter)
    LOGGER.addHandler(log_file)
    LOGGER.propagate = False

    config._sertun_log_path = log_path
    LOGGER.info("SESSION start")
    LOGGER.info("log_file=%s", log_path)
    LOGGER.info(
        "python=%s pytest=%s platform=%s",
        platform.python_version(),
        pytest.__version__,
        platform.platform(),
    )
    LOGGER.info(
        "serial_port=%s tcp_port=%s startup_delay=%.3fs connect_timeout=%.3fs",
        config.getoption("--serial-port"),
        config.getoption("--tcp-port"),
        config.getoption("--startup-delay"),
        config.getoption("--connect-timeout"),
    )
    LOGGER.info(
        "enable_large_tests=%s",
        config.getoption("--enable-large-tests"),
    )
    LOGGER.info(
        "ping_pong_count=%s ping_pong_log_every=%s",
        config.getoption("--ping-pong-count"),
        config.getoption("--ping-pong-log-every"),
    )


def summarize_parameter(value):
    if isinstance(value, bytes):
        return (
            f"bytes(length={len(value)},"
            f"sha256={hashlib.sha256(value).hexdigest()})"
        )
    return repr(value)


def pytest_runtest_logstart(nodeid, location):
    LOGGER.info("TEST start nodeid=%s", nodeid)


def pytest_runtest_makereport(item, call):
    if call.when != "call":
        return
    parameters = getattr(item, "callspec", None)
    if parameters:
        summary = ", ".join(
            f"{name}={summarize_parameter(value)}"
            for name, value in parameters.params.items()
        )
        LOGGER.info("TEST parameters nodeid=%s %s", item.nodeid, summary)


def pytest_runtest_logreport(report):
    if report.when == "call":
        LOGGER.info(
            "TEST result nodeid=%s outcome=%s duration=%.6fs",
            report.nodeid,
            report.outcome.upper(),
            report.duration,
        )


def pytest_collection_modifyitems(config, items):
    if config.getoption("--enable-large-tests"):
        return

    skip_large = pytest.mark.skip(
        reason="large 1 MiB payload tests are disabled by default"
    )
    for item in items:
        if "large_transfer" in item.keywords or "large_payload" in item.keywords:
            item.add_marker(skip_large)


def pytest_sessionfinish(session, exitstatus):
    LOGGER.info(
        "SESSION finish exitstatus=%s tests_collected=%s",
        exitstatus,
        session.testscollected,
    )


def connect_with_retry(port, timeout):
    deadline = time.monotonic() + timeout
    while True:
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=1)
            connection.settimeout(10)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return connection
        except OSError:
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"QEMU UART endpoint 127.0.0.1:{port} was not ready "
                    f"within {timeout:.1f}s"
                )
            time.sleep(0.25)


@pytest.fixture(scope="session")
def qemu_uart_bridge(request):
    uart0_port = request.config.getoption("--qemu-uart0-port")
    uart2_port = request.config.getoption("--qemu-uart2-port")
    status_path = Path(
        request.config.getoption("--qemu-status-log")
    ).resolve()
    timeout = request.config.getoption("--connect-timeout")

    LOGGER.info(
        "QEMU connecting uart0=127.0.0.1:%s uart2=127.0.0.1:%s",
        uart0_port,
        uart2_port,
    )
    with connect_with_retry(uart0_port, timeout) as uart0:
        with connect_with_retry(uart2_port, timeout) as uart2:
            ready_deadline = time.monotonic() + timeout
            while time.monotonic() < ready_deadline:
                if status_path.exists() and "ready: UART0 <-> UART2" in (
                    status_path.read_text(encoding="utf-8", errors="replace")
                ):
                    break
                time.sleep(0.25)
            else:
                raise TimeoutError(
                    f"QEMU firmware did not report ready in {status_path}"
                )

            for endpoint in (uart0, uart2):
                endpoint.settimeout(0.05)
                while True:
                    try:
                        if not endpoint.recv(4096):
                            break
                    except TimeoutError:
                        break
                endpoint.settimeout(10)

            LOGGER.info("QEMU firmware UART bridge ready")
            yield {"uart0": uart0, "uart2": uart2}
            LOGGER.info("QEMU UART bridge connections closing")


@pytest.fixture(scope="session")
def tunnel(request):
    serial_port = request.config.getoption("--serial-port")
    tcp_port = request.config.getoption("--tcp-port")
    connect_timeout = request.config.getoption("--connect-timeout")
    startup_delay = request.config.getoption("--startup-delay")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", tcp_port))
        server.listen(1)
        server.settimeout(connect_timeout)
        LOGGER.info(
            "SERVER listening address=0.0.0.0 port=%s backlog=1", tcp_port
        )
        LOGGER.info("SERVER startup_delay begin seconds=%.3f", startup_delay)
        time.sleep(startup_delay)
        LOGGER.info("SERVER startup_delay complete; waiting for device")

        with serial.Serial(
            serial_port,
            baudrate=115200,
            timeout=15,
            write_timeout=30,
        ) as tunnel_serial:
            LOGGER.info(
                "SERIAL opened port=%s baudrate=%s timeout=%s write_timeout=%s",
                tunnel_serial.port,
                tunnel_serial.baudrate,
                tunnel_serial.timeout,
                tunnel_serial.write_timeout,
            )
            accept_deadline = time.monotonic() + connect_timeout
            server.settimeout(1.0)
            probe = b"\xA5"
            while True:
                try:
                    connection, address = server.accept()
                    break
                except TimeoutError:
                    if time.monotonic() >= accept_deadline:
                        raise TimeoutError(
                            f"device did not connect to TCP port {tcp_port} "
                            f"within {connect_timeout:.3f}s"
                        )
                    written = tunnel_serial.write(probe)
                    LOGGER.info(
                        "SERVER reconnect_probe serial_bytes=%s remaining=%.3fs",
                        written,
                        accept_deadline - time.monotonic(),
                    )
            with connection:
                connection.settimeout(15)
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                LOGGER.info(
                    "SERVER device_connected peer=%s:%s local=%s:%s tcp_nodelay=1",
                    address[0],
                    address[1],
                    *connection.getsockname(),
                )
                time.sleep(0.5)
                connection.settimeout(0.05)
                drained_probe_bytes = 0
                while True:
                    try:
                        chunk = connection.recv(4096)
                    except TimeoutError:
                        break
                    if not chunk:
                        break
                    drained_probe_bytes += len(chunk)
                connection.settimeout(15)
                LOGGER.info(
                    "SERVER reconnect_probe_drain bytes=%s", drained_probe_bytes
                )
                tunnel_serial.reset_input_buffer()
                tunnel_serial.reset_output_buffer()
                LOGGER.info("SERIAL buffers reset")

                def prime_usb_cdc():
                    token = b"\x00SerTun32-USB-TX-ready\xff"
                    tunnel_serial.reset_input_buffer()
                    written = tunnel_serial.write(token)
                    if written != len(token):
                        raise RuntimeError(
                            f"wrote {written} of {len(token)} USB sync bytes"
                        )
                    received = receive_exact(connection.recv, len(token))
                    if received != token:
                        raise RuntimeError("USB CDC synchronization mismatch")
                    LOGGER.info(
                        "SYNC usb_to_tcp bytes=%s sha256=%s",
                        len(token),
                        hashlib.sha256(token).hexdigest(),
                    )
                    time.sleep(0.05)

                prime_usb_cdc()
                guard_state = {
                    "sequence": 1,
                    "pending": b"SerTun32-guard-0000".ljust(64, b"\x00"),
                }
                next_guard = b"SerTun32-guard-0001".ljust(64, b"\x00")
                connection.sendall(guard_state["pending"])
                connection.sendall(next_guard)
                received = receive_exact(tunnel_serial.read, 64)
                if received != guard_state["pending"]:
                    raise RuntimeError("USB CDC TX pipeline initialization mismatch")
                LOGGER.info(
                    "SYNC tcp_to_usb guard_bytes=64 sha256=%s",
                    hashlib.sha256(received).hexdigest(),
                )
                guard_state["pending"] = next_guard

                def rotate_usb_tx_guard(log_rotation=True):
                    previous = guard_state["pending"]
                    guard_state["sequence"] += 1
                    current = (
                        f"SerTun32-guard-{guard_state['sequence']:04d}".encode()
                    ).ljust(64, b"\x00")
                    guard_state["pending"] = current
                    if log_rotation:
                        LOGGER.info(
                            "SYNC guard_rotate sequence=%s previous_sha256=%s "
                            "next_sha256=%s",
                            guard_state["sequence"],
                            hashlib.sha256(previous).hexdigest(),
                            hashlib.sha256(current).hexdigest(),
                        )
                    return previous, current

                try:
                    yield {
                        "serial": tunnel_serial,
                        "socket": connection,
                        "peer": address,
                        "prime": prime_usb_cdc,
                        "rotate_usb_tx_guard": rotate_usb_tx_guard,
                    }
                finally:
                    LOGGER.info("SERVER closing device connection peer=%s:%s", *address)
                    connection.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_LINGER,
                        struct.pack("ii", 1, 0),
                    )
        LOGGER.info("SERIAL closed port=%s", serial_port)
    LOGGER.info("SERVER stopped port=%s", tcp_port)
