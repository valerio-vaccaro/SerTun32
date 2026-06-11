/**
 * @file config.h
 * @brief Compile-time configuration: board pin map and project defaults.
 *
 * Everything that is hardware- or deployment-specific lives here so the rest
 * of the firmware stays board-agnostic.
 *
 * Board profiles are selected by the PlatformIO environment. Display pins
 * remain in platformio.ini because TFT_eSPI consumes them as build flags.
 */
#ifndef SERTUN32_CONFIG_H
#define SERTUN32_CONFIG_H

/* ------------------------------------------------------------------------- */
/*  Firmware identity                                                        */
/* ------------------------------------------------------------------------- */
#define FIRMWARE_VERSION    "1.2.0"

#if defined(SERTUN_BOARD_TDISPLAY_S3)
#define FIRMWARE_TARGET     "T-Display-S3"
#elif defined(SERTUN_BOARD_TDISPLAY)
#define FIRMWARE_TARGET     "T-Display"
#elif defined(SERTUN_QEMU)
#define FIRMWARE_TARGET     "QEMU-ESP32"
#else
#error "Select a SerTun32 PlatformIO environment"
#endif

/* ------------------------------------------------------------------------- */
/*  Display geometry (after rotation)                                        */
/* ------------------------------------------------------------------------- */
/* Both physical displays are rotated to landscape. */
#define TFT_ROTATION        1       /* 1 = landscape                         */
#if defined(SERTUN_BOARD_TDISPLAY_S3)
#define SCREEN_WIDTH        320
#define SCREEN_HEIGHT       170
#elif defined(SERTUN_BOARD_TDISPLAY)
#define SCREEN_WIDTH        240
#define SCREEN_HEIGHT       135
#endif

/* ------------------------------------------------------------------------- */
/*  Board power-enable                                                       */
/* ------------------------------------------------------------------------- */
/* On the T-Display-S3 GPIO15 gates power to the LCD (and the LDO that feeds
 * it). It MUST be driven HIGH at boot or the screen stays dark. */
#if defined(SERTUN_BOARD_TDISPLAY_S3)
#define BOARD_POWER_ON_PIN  15
#endif

/* ------------------------------------------------------------------------- */
/*  Buttons                                                                  */
/* ------------------------------------------------------------------------- */
/* The T-Display-S3 has two buttons: GPIO0 (BOOT) and GPIO14 (KEY). We use the
 * BOOT button: pressing it while the tunnel is running forces the device back
 * into configuration (captive-portal) mode. Both are active LOW. */
#define BUTTON_CONFIG_PIN   0       /* GPIO0 BOOT (use 14 for the KEY button) */
#define BUTTON_ACTIVE_LEVEL 0       /* 0 = pressed pulls the pin LOW         */
#define BUTTON_DEBOUNCE_MS  50
#define BUTTON_HOLD_MS      60      /* must stay pressed this long to count  */

/* ------------------------------------------------------------------------- */
/*  Tunnel endpoint = the USB-C port (native USB-CDC `Serial`)               */
/* ------------------------------------------------------------------------- */
/* The "serial port" that gets bridged to the TCP socket is the USB-C port -
 * i.e. the COM/tty port your PC sees over the USB-C cable. Nothing to wire:
 * open that COM port on the PC and every byte is tunnelled to the socket and
 * back. (USB-CDC ignores the baud rate; it is kept only for reference.) */
#define TUNNEL_BAUD         115200
/* Large USB-CDC ring buffers: the ESP32-S3 USB peripheral does NOT apply USB
 * backpressure when full - it overwrites unread data and corrupts the stream.
 * The 1 MiB RX queue is allocated from the T-Display-S3's PSRAM. */
#if defined(SERTUN_BOARD_TDISPLAY_S3)
#define TUNNEL_RX_BUFFER    (1024U * 1024U) /* PSRAM-backed, max burst 1 MiB  */
#define TUNNEL_TX_BUFFER    (16U * 1024U)
#elif defined(SERTUN_BOARD_TDISPLAY)
/* The original ESP32 T-Display has no PSRAM in its standard board profile. */
#define TUNNEL_RX_BUFFER    (64U * 1024U)
#define TUNNEL_TX_BUFFER    (8U * 1024U)
#elif defined(SERTUN_QEMU)
#define TUNNEL_RX_BUFFER    (16U * 1024U)
#define TUNNEL_TX_BUFFER    (16U * 1024U)
#endif

/* No serial debug logging is emitted anywhere: the USB-C port carries only
 * tunnel data, and every status / diagnostic message is shown on the display.
 * This keeps the tunnelled stream byte-exact. */

/* ------------------------------------------------------------------------- */
/*  Captive-portal access-point defaults                                     */
/* ------------------------------------------------------------------------- */
/* The AP SSID gets a per-device suffix appended at runtime (from the MAC) so
 * that several units can coexist, e.g. "SerTun32-A1B2". */
#define AP_SSID_PREFIX      "SerTun32-"
#define AP_PASSWORD         "sertun32"   /* >= 8 chars required by WPA2       */
#define AP_DNS_PORT         53
#define AP_HTTP_PORT        80

/* ------------------------------------------------------------------------- */
/*  Behavioural timing                                                       */
/* ------------------------------------------------------------------------- */
#define WIFI_CONNECT_TIMEOUT_MS   20000  /* give up joining the WiFi after.. */
#define TCP_RECONNECT_INTERVAL_MS 3000   /* wait between socket retries      */
#define TCP_WRITE_STALL_TIMEOUT_MS 1000  /* close socket after no TX progress */
#define STATS_UPDATE_INTERVAL_MS  2000   /* refresh on-screen statistics     */
#define TCP_RX_BUFFER_SIZE        2048   /* per-pump transfer chunk size     */

/* ------------------------------------------------------------------------- */
/*  NVS (Preferences) storage keys                                           */
/* ------------------------------------------------------------------------- */
#define NVS_NAMESPACE       "sertun32"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"
#define NVS_KEY_TCP_IP      "tcpip"
#define NVS_KEY_TCP_PORT    "tcpport"

#endif /* SERTUN32_CONFIG_H */
