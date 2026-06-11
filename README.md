# SerTun32

**Serial ↔ TCP tunnel for LilyGo T-Display S3 boards**, with a captive-portal
configuration UI and an LVGL status screen. A headless ESP32 QEMU profile is
also included for testing the bidirectional byte pump without hardware.

Current firmware version: **1.2.0**. It is shown persistently in the display
header and on the captive-portal configuration page.

SerTun32 turns a supported T-Display S3 into a transparent bridge between its
USB serial interface and a TCP socket over WiFi. Point it at a TCP host/port
and every byte is forwarded in both directions. The on-board TFT shows the
live link status and message counters.

T-display is supporte with some limits on speed and message size.

## Features

- **Captive-portal WiFi setup** – on first boot (or after pressing the button)
  the device starts an access point and a captive portal. The AP SSID and
  password are printed on the display. Any device that joins is redirected to a
  web form. The form **scans for nearby WiFi networks** and offers them as a
  pick-list (you can still type a hidden SSID).
- **Configurable TCP socket** – the same form asks for the destination TCP
  **IP address** and **port** that the serial port will be tunnelled to.
- **Bidirectional tunnel** – once connected to WiFi, bytes are pumped
  `USB ↔ TCP` 8-bit transparently (binary-clean, verified for all 256 byte
  values incl. NUL / 0xFF / XON-XOFF).
- **LVGL status UI** – a clean [LVGL](https://lvgl.io) screen with a black
  background and a **bitcoin-orange header**. Only the large status line is
  **colour-coded by state** (yellow = setup/busy, green = TUNNEL OK, red =
  error); the separator rule and info text are white. In tunnel mode it shows
  the **tunnel target IP:port** plus, every couple of seconds, the running
  statistics: number of messages (and bytes) serial→socket and socket→serial.
  All status is shown here — nothing is printed on the serial port.
- **Button to reconfigure** – pressing the BOOT button at any time returns the
  device to the captive-portal setup page.
- **Persistent configuration** – WiFi credentials and the TCP target are stored
  in NVS (flash) and survive reboots.

## Hardware

| Profile | Display | Tunnelled port | RX queue |
|---|---|---|---|
| T-Display-S3 | 170×320, 8-bit parallel | Native USB CDC | 1 MiB in PSRAM |
| Original T-Display | 135×240, SPI | USB-to-UART bridge | 64 KiB internal RAM |
| QEMU ESP32 | Headless | Emulated UART0 ↔ UART2 | 16 KiB per UART |

> ⚠️ **Pin map.** The display/control pins are set in `platformio.ini`
> (mirroring the official `Setup206_LilyGo_T_Display_S3`). Board-level pins
> (power, button) are in `include/config.h`. Note GPIO 15 must be HIGH or the
> panel stays dark — the firmware handles this in `setup()`.

### Which "serial port" is tunnelled?

The tunnelled serial port **is the USB-C port** — the COM/tty port your computer
sees when you plug in the USB-C cable. There is **nothing to wire**: open that
COM port on your PC (any terminal/app, e.g. PuTTY, `screen`, pySerial) and every
byte is forwarded to the TCP socket, and everything the socket sends comes back
out the COM port.

The USB-C port carries **only** the tunnelled data — there is intentionally no
debug logging on any serial port. Every status and diagnostic message is shown
on the **display** instead, which keeps the tunnel stream byte-exact.

> Note: because the USB stream is raw tunnel data, do not use
> `pio device monitor` as a console while the tunnel is active — watch the TFT
> for status instead.

## Project layout

```
SerTun32/
├── platformio.ini      # Board, libraries and TFT_eSPI / LVGL build flags
├── include/
│   ├── config.h        # Pin map, defaults, timeouts, NVS keys
│   └── lv_conf.h       # LVGL v8 configuration
├── src/
│   └── main.cpp        # Firmware: state machine, portal, tunnel, UI
├── README.md           # This file
└── SerTun32.psf        # Original specification (kept up to date)
```

## How it works

The firmware is a three-state machine:

| State            | What happens                                                                 |
|------------------|------------------------------------------------------------------------------|
| `CONFIG`         | Runs AP + DNS + HTTP captive portal. Display shows AP SSID/pass + portal URL. |
| `CONNECTING`     | Joins the configured WiFi. On timeout, falls back to `CONFIG`.                |
| `TUNNEL`         | Opens the TCP socket and bridges it to `Serial1`; shows status + stats.      |

- On boot, if a complete configuration exists in NVS, the device goes straight
  to `CONNECTING`; otherwise it starts in `CONFIG`.
- Pressing **GPIO 0** while not in `CONFIG` forces the device back to the setup
  portal.
- The TCP socket auto-reconnects (every few seconds) if it drops, and the WiFi
  link is re-joined automatically if it is lost.

### Display states

Only the status line is colour-coded by state (yellow / green / red):

- **SETUP** (yellow) – join AP `SerTun32-XXXX`, password `sertun32`, then open
  the shown IP in a browser.
- **CONNECTING** (yellow) – joining WiFi.
- **WIFI FAIL** (red) – could not join; returns to setup.
- **TUNNEL OK** (green) – socket connected, data flowing; stats updated every
  ~2 s.
- **NO SOCKET** (red) – WiFi is up but the TCP socket is down (retrying).

## Replacing a `socat` bridge (throughput & flow control)

This firmware can stand in for a host-side bridge such as:

```
socat TCP4:<host>:<port>,connect-timeout=5  OPEN:/dev/ttyGS0,rawer
```

Equivalences: it is a **TCP client**, the device serial side is **raw / 8-bit
clean** (no tty line discipline on the MCU — effectively `rawer`), and the
USB-C `/dev/ttyACM*` is the analog of the `ttyGS0` gadget.

Differences to be aware of:

- **No USB backpressure.** A Linux USB gadget NAKs the host when its buffers
  fill, so `socat` never loses data on arbitrarily large transfers. The
  ESP32-S3 USB-CDC does **not** apply backpressure — if the host sends a burst
  larger than the firmware can drain to WiFi, the RX ring overflows and the
  stream corrupts. To cover this, the USB RX/TX ring buffers are enlarged
  (`TUNNEL_RX_BUFFER` = 1 MiB in PSRAM, `TUNNEL_TX_BUFFER` = 16 KiB in
  `config.h`). This permits a host message/burst of up to 1 MiB, but a
  *sustained* stream faster than the WiFi link can carry will eventually drop.
- **Target is over WiFi**, not `127.0.0.1`. Point it at the real IP:port; WiFi
  is now the throughput bottleneck (which is what makes the buffer size matter).
- **Auto-reconnect.** Unlike one-shot `socat`, the firmware retries the WiFi
  join and the TCP socket forever.
- **`TCP_NODELAY` is on** (lower latency; `socat` leaves Nagle on by default).
- **No `-v` data logging.** Status is on the display only.

## Building & flashing

This project uses [PlatformIO](https://platformio.org/) inside a Python
virtual environment.

### 1. Create the venv and install PlatformIO

```bash
python3 -m venv venv
./venv/bin/pip install --upgrade pip
./venv/bin/pip install platformio
```

### 2. Select and build a target

PlatformIO environments select the required firmware:

| Environment | Target |
|---|---|
| `lilygo-t-display-s3` | T-Display-S3, default |
| `lilygo-t-display` | Original ESP32 T-Display (with limitation) |
| `qemu-esp32` | Espressif QEMU, headless UART bridge |

```bash
./venv/bin/pio run
./venv/bin/pio run -e lilygo-t-display
./venv/bin/pio run -e qemu-esp32
```

Build output is stored in `.pio/build/<environment>/firmware.bin`.

### 3. Upload a physical board

T-Display-S3:

```bash
./venv/bin/pio run -e lilygo-t-display-s3 \
  --target upload --upload-port /dev/ttyACM0
```

Original T-Display:

```bash
./venv/bin/pio run -e lilygo-t-display \
  --target upload --upload-port /dev/ttyUSB0
```

The native USB (USB Serial/JTAG) needs `upload_speed = 115200` and
`upload_flags = --no-stub` — both are preconfigured in `platformio.ini`.
Without them esptool fails with *"No serial data received"* (the port can't
switch baud and the flasher stub re-enumerates the USB device mid-flash).

> If the board does not enter download mode automatically, hold **BOOT**, tap
> **RST**, then release **BOOT** before uploading.
>
> Note: there is no serial console — the USB port is the data tunnel. Watch the
> TFT for status instead of `pio device monitor`.

### 4. Build and run QEMU

Use [Espressif's QEMU fork](https://github.com/espressif/qemu). The regular
Linux `qemu-system-xtensa` package does not include the required `esp32`
machine. With ESP-IDF installed:

```bash
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
. "$IDF_PATH/export.sh"
```

Build a complete 4 MiB flash image:

```bash
./scripts/build_qemu.sh
```

Run it:

```bash
./scripts/run_qemu.sh
```

To select a binary outside `PATH`:

```bash
QEMU_ESP32=/path/to/qemu-system-xtensa ./scripts/run_qemu.sh
```

The runner exposes:

- UART0 data: `tcp://127.0.0.1:30121`
- UART2 data: `tcp://127.0.0.1:30122`
- UART1 status: `.pio/build/qemu-esp32/uart1-status.log`

Bytes sent to either data endpoint appear at the other. For example, connect
one terminal to each endpoint:

```bash
nc 127.0.0.1 30121
nc 127.0.0.1 30122
```

Espressif QEMU does not emulate the ESP32 WiFi radio. Its available OpenCores
Ethernet device is not used by Arduino `WiFiClient`, so captive-portal and
WiFi/TCP behavior remains in the physical profiles. The QEMU profile validates
the buffered bidirectional transport over two emulated UARTs.

### 5. End-to-end tunnel test

Configure the device to connect to the test computer on TCP port `30121`, then
run:

```bash
./venv/bin/pip install -r requirements-test.txt
./venv/bin/pytest -v --serial-port=/dev/ttyACM0 --tcp-port=30121
```

The pytest session starts the TCP server automatically and waits for the
device. It allows a 10-second startup grace period after a firmware update
before accepting the tunnel; override this with `--startup-delay=SECONDS`.
Parametrized cases check varied sizes, ASCII and Unicode encodings, every byte
value, NUL/control-byte patterns, deterministic random data, and exact 1 MiB
payloads in both directions using SHA-256 comparisons. The 1 MiB cases are
skipped by default because they can destabilize the original T-Display; enable
them explicitly with `--enable-large-tests`.

All server, serial, connection, synchronization, test-vector, SHA-256, timing,
throughput, packet-rate, and pass/fail information is logged to the console and
`test-results/sertun32-pytest.log`. Override the file with
`--sertun-log=PATH`. While waiting for a connection, pytest sends a one-byte
USB probe once per second to make the firmware detect and replace any stale TCP
test socket left by a previous session.

Timed throughput and burst tests are included in the same suite. Use `-s` to
show elapsed time, KiB/s, and application-write packets/s for small, repeated,
large, and 10/50/100-packet vectors:

```bash
./venv/bin/pytest -v -s -m performance \
  --serial-port=/dev/ttyACM0 --tcp-port=30121
```

For TCP-to-USB measurements the harness uses a 64-byte test-only guard to
advance the ESP32 HWCDC final FIFO chunk. Guard bytes are integrity-checked but
excluded from the reported payload byte and packet counts.

Run three distinct 1 MiB messages in each direction with:

```bash
./venv/bin/pytest -v -s -m large_transfer --enable-large-tests \
  --serial-port=/dev/ttyACM0 --tcp-port=30121
```

Each message is verified independently with SHA-256. The log includes
per-message timing and aggregate throughput for both directions.

Run strict one-byte ping-pong round trips with:

```bash
./venv/bin/pytest -v -s -m pingpong \
  --serial-port=/dev/ttyACM0 --tcp-port=30121
```

Each iteration completes USB→TCP verification and TCP→USB verification before
the next byte starts. By default it sends 1,000 messages and logs progress
every 100 iterations. Override
these defaults with `--ping-pong-count=N` and `--ping-pong-log-every=N`.

## First-time setup walkthrough

1. Flash the firmware and power the board. The screen shows **SETUP MODE** with
   an AP name like `SerTun32-A1B2` and password `sertun32`.
2. On a phone/laptop, join that WiFi network. A captive-portal page should pop
   up automatically (or open the `http://…` address shown on the display).
3. Enter your **WiFi SSID/password** and the **TCP host IP and port** to tunnel
   to, then tap **Save & Connect**.
4. The device joins your WiFi and connects the socket. The screen turns
   **TUNNEL OK** (green) and starts showing message counters.
5. To change anything later, press the **BOOT** button to return to setup.

## Configuration reference

Edit `include/config.h` to change defaults:

| Setting                     | Meaning                                            |
|-----------------------------|----------------------------------------------------|
| `BUTTON_CONFIG_PIN`         | GPIO for the "back to config" button (default 0).  |
| `TUNNEL_UART_RX/TX_PIN`     | UART pins for the tunnelled serial port.           |
| `TUNNEL_UART_BAUD`          | Baud rate of the tunnelled serial port.            |
| `AP_SSID_PREFIX` / `AP_PASSWORD` | Captive-portal AP name prefix / password.     |
| `WIFI_CONNECT_TIMEOUT_MS`   | How long to try joining WiFi before giving up.     |
| `TCP_RECONNECT_INTERVAL_MS` | Delay between TCP reconnect attempts.              |
| `STATS_UPDATE_INTERVAL_MS`  | How often the on-screen statistics refresh.        |

Display pins and board selection live in `platformio.ini` as per-environment
`TFT_*` and `SERTUN_*` build flags.

## License

MIT
