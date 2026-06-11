/**
 * @file main.cpp
 * @brief SerTun32 - Serial <-> TCP tunnel and QEMU UART bridge.
 *
 * High level behaviour
 * ====================
 * The firmware is a small state machine with three states:
 *
 *   STATE_CONFIG      The device runs a WiFi access point and a captive
 *                     portal. The AP SSID / password are printed on the TFT.
 *                     A web form lets the user enter the station WiFi
 *                     credentials and the destination TCP socket (IP + port).
 *
 *   STATE_CONNECTING  The device tries to join the configured WiFi network.
 *                     On success it advances to STATE_TUNNEL; on timeout it
 *                     falls back to STATE_CONFIG.
 *
 *   STATE_TUNNEL      The device opens a TCP connection to the configured
 *                     socket and bridges it to the USB-C serial port (Serial,
 *                     USB-CDC): bytes read from the USB COM port are written to
 *                     the socket and vice-versa. The TFT shows the link status
 *                     plus, every few seconds, the running message counters.
 *
 * Pressing the BOOT button at any time while tunnelling returns the device to
 * STATE_CONFIG so the network / socket can be re-configured.
 *
 * The graphics are drawn with LVGL (kept deliberately simple: a title, a big
 * status line and a multi-line info/stats block) rendered through TFT_eSPI.
 *
 * Serial usage
 * ============
 *   Serial (native USB-CDC, the USB-C port) carries ONLY the tunnelled data.
 *   There is intentionally no debug logging on any serial port: every status
 *   and diagnostic message is shown on the TFT display instead, so the USB
 *   stream stays a clean, byte-exact tunnel.
 */

#include <Arduino.h>

#include "config.h"

#ifdef SERTUN_QEMU

/*
 * Espressif QEMU does not emulate the ESP32 WiFi radio. This profile therefore
 * exercises the byte-pump core as a headless UART0 <-> UART2 bridge. UART1 is
 * reserved for status so neither data stream is contaminated by diagnostics.
 */
static HardwareSerial &dataA = Serial;
static HardwareSerial &statusPort = Serial1;
static HardwareSerial &dataB = Serial2;
static uint64_t bytesAToB = 0;
static uint64_t bytesBToA = 0;

static void pumpUart(HardwareSerial &source, HardwareSerial &destination,
                     uint64_t &counter) {
    static uint8_t buffer[TCP_RX_BUFFER_SIZE];
    int available = source.available();
    if (available <= 0) return;

    size_t count = source.readBytes(
        buffer, min(available, static_cast<int>(sizeof(buffer))));
    size_t sent = 0;
    while (sent < count) {
        size_t written = destination.write(buffer + sent, count - sent);
        if (written > 0) sent += written;
        else delay(1);
    }
    counter += count;
}

void setup() {
    dataA.setRxBufferSize(TUNNEL_RX_BUFFER);
    dataA.setTxBufferSize(TUNNEL_TX_BUFFER);
    dataB.setRxBufferSize(TUNNEL_RX_BUFFER);
    dataB.setTxBufferSize(TUNNEL_TX_BUFFER);

    dataA.begin(TUNNEL_BAUD);
    statusPort.begin(TUNNEL_BAUD);
    dataB.begin(TUNNEL_BAUD);
    dataA.setTimeout(5);
    dataB.setTimeout(5);

    statusPort.printf(
        "SerTun32 v%s target=%s ready: UART0 <-> UART2\r\n",
        FIRMWARE_VERSION, FIRMWARE_TARGET);
}

void loop() {
    pumpUart(dataA, dataB, bytesAToB);
    pumpUart(dataB, dataA, bytesBToA);

    static uint32_t lastStatus = 0;
    uint32_t now = millis();
    if (now - lastStatus >= STATS_UPDATE_INTERVAL_MS) {
        lastStatus = now;
        statusPort.printf(
            "bytes UART0->UART2=%llu UART2->UART0=%llu\r\n",
            static_cast<unsigned long long>(bytesAToB),
            static_cast<unsigned long long>(bytesBToA));
    }
    delay(0);
}

#else

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <esp_log.h>

/* ========================================================================= */
/*  Application state                                                        */
/* ========================================================================= */

enum AppState {
    STATE_CONFIG,       /**< Captive portal active, waiting for user input.  */
    STATE_CONNECTING,   /**< Joining the configured WiFi network.            */
    STATE_TUNNEL        /**< Connected; bridging UART <-> TCP socket.        */
};

static AppState g_state = STATE_CONFIG;

/** Persisted configuration loaded from / saved to NVS. */
struct DeviceConfig {
    String wifiSsid;    /**< Station SSID to join.                           */
    String wifiPass;    /**< Station password.                               */
    String tcpIp;       /**< Destination TCP host (dotted IPv4).             */
    uint16_t tcpPort;   /**< Destination TCP port.                           */
};

static DeviceConfig g_cfg;

/** Tunnel statistics (the numbers shown on screen). */
struct TunnelStats {
    uint32_t msgsSerialToSocket;  /**< read events UART  -> socket           */
    uint32_t msgsSocketToSerial;  /**< read events socket -> UART            */
    uint64_t bytesSerialToSocket; /**< total bytes UART  -> socket           */
    uint64_t bytesSocketToSerial; /**< total bytes socket -> UART            */
};

static TunnelStats g_stats = {0, 0, 0, 0};

/* ========================================================================= */
/*  Globals: drivers, servers, LVGL objects                                  */
/* ========================================================================= */

static TFT_eSPI         tft = TFT_eSPI();
static Preferences      prefs;
static DNSServer        dnsServer;
static WebServer        httpServer(AP_HTTP_PORT);
static WiFiClient       tcpClient;

static String           g_apSsid;          /* AP SSID incl. MAC suffix.      */
static IPAddress        g_apIp;            /* AP gateway IP (portal target). */
static String           g_ssidOptions;     /* <option> list of scanned SSIDs */

/* LVGL display plumbing. */
/* Render stripe height in lines. 20 lines * 320 px * 2 B = ~12.5 kB, a good
 * balance between RAM use and flush overhead on the ESP32-S3. */
#define LV_DRAW_BUF_LINES 20
static lv_disp_draw_buf_t s_drawBuf;
static lv_color_t         s_lvBuf[SCREEN_WIDTH * LV_DRAW_BUF_LINES];

/* LVGL widgets that get updated as state changes. */
static lv_obj_t *ui_header = nullptr;   /* top accent bar                    */
static lv_obj_t *ui_title  = nullptr;   /* "SerTun32" in the header          */
static lv_obj_t *ui_rule   = nullptr;   /* jade accent separator line        */
static lv_obj_t *ui_status = nullptr;   /* big state-coloured status text     */
static lv_obj_t *ui_info   = nullptr;   /* large multi-line info / stats     */

#if defined(SERTUN_BOARD_TDISPLAY)
static constexpr uint8_t UI_HEADER_HEIGHT = 24;
static constexpr const lv_font_t *UI_TITLE_FONT = &lv_font_montserrat_16;
static constexpr const lv_font_t *UI_STATUS_FONT = &lv_font_montserrat_20;
static constexpr const lv_font_t *UI_INFO_FONT = &lv_font_montserrat_16;
static constexpr int UI_STATUS_Y = 27;
static constexpr int UI_RULE_Y = 54;
static constexpr int UI_INFO_Y = 58;
static constexpr int UI_INFO_WIDTH_MARGIN = 10;
static constexpr int UI_STATUS_EXTRA_Y = 0;
static constexpr int UI_INFO_LINE_SPACE = 0;
#else
static constexpr uint8_t UI_HEADER_HEIGHT = 32;
static constexpr const lv_font_t *UI_TITLE_FONT = &lv_font_montserrat_20;
static constexpr const lv_font_t *UI_STATUS_FONT = &lv_font_montserrat_28;
static constexpr const lv_font_t *UI_INFO_FONT = &lv_font_montserrat_18;
static constexpr int UI_STATUS_Y = 38;
static constexpr int UI_RULE_Y = 74;
static constexpr int UI_INFO_Y = 82;
static constexpr int UI_INFO_WIDTH_MARGIN = 12;
static constexpr int UI_STATUS_EXTRA_Y = 0;
static constexpr int UI_INFO_LINE_SPACE = 2;
#endif

/* ---- Blockstream Jade inspired palette --------------------------------- */
/* Jade's UI is black with a distinctive jade-green accent and clean,         */
/* state-coloured text rather than heavy coloured blocks.                     */
#define COLOR_BG       lv_color_hex(0x000000)   /* pure black background       */
#define COLOR_BITCOIN  lv_color_hex(0xF7931A)   /* bitcoin orange = header bar */
#define COLOR_OK       lv_color_hex(0x00B377)   /* jade green = good           */
#define COLOR_WARN     lv_color_hex(0xFFC400)   /* yellow = busy / setup       */
#define COLOR_ERR      lv_color_hex(0xE23A3A)   /* red = error                 */
#define COLOR_TXT      lv_color_hex(0xFFFFFF)   /* primary text (info + rule)  */

/* Timers / latches. */
static uint32_t g_lastStatsUpdate   = 0;
static uint32_t g_lastTcpAttempt    = 0;
static uint32_t g_connectStartTime  = 0;
static bool     g_socketWasUp       = false;

/* ========================================================================= */
/*  Forward declarations                                                     */
/* ========================================================================= */
static void enterConfigState();
static void enterConnectingState();
static void enterTunnelState();

/* ========================================================================= */
/*  Persistent configuration helpers                                         */
/* ========================================================================= */

/** Load the configuration from NVS into g_cfg (with sane empty defaults). */
static void loadConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    g_cfg.wifiSsid = prefs.getString(NVS_KEY_SSID, "");
    g_cfg.wifiPass = prefs.getString(NVS_KEY_PASS, "");
    g_cfg.tcpIp    = prefs.getString(NVS_KEY_TCP_IP, "");
    g_cfg.tcpPort  = prefs.getUShort(NVS_KEY_TCP_PORT, 0);
    prefs.end();
}

/** Persist g_cfg to NVS. */
static void saveConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putString(NVS_KEY_SSID, g_cfg.wifiSsid);
    prefs.putString(NVS_KEY_PASS, g_cfg.wifiPass);
    prefs.putString(NVS_KEY_TCP_IP, g_cfg.tcpIp);
    prefs.putUShort(NVS_KEY_TCP_PORT, g_cfg.tcpPort);
    prefs.end();
}

/** True when we have everything required to attempt a tunnel. */
static bool configIsComplete() {
    return g_cfg.wifiSsid.length() > 0 &&
           g_cfg.tcpIp.length()   > 0 &&
           g_cfg.tcpPort          > 0;
}

/* ========================================================================= */
/*  LVGL <-> TFT_eSPI glue                                                    */
/* ========================================================================= */

/**
 * @brief LVGL flush callback: push a rendered rectangle to the panel.
 */
static void lvDispFlush(lv_disp_drv_t *disp, const lv_area_t *area,
                        lv_color_t *colorP) {
    const uint32_t w = (area->x2 - area->x1 + 1);
    const uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&colorP->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

/* ========================================================================= */
/*  UI construction & updates                                                */
/* ========================================================================= */

/**
 * @brief Build the (single) LVGL screen used by every state.
 *
 * Blockstream Jade inspired: black background, jade-green header strip, big
 * state-coloured status text and a thin jade accent rule.
 *
 * Layout (landscape 320x170):
 *   +==========================================+  <- jade-green header bar
 *   |               SerTun32                    |     (black title, 20px)
 *   +------------------------------------------+
 *   |   (icon)  STATUS TEXT                     |     (28px, colour = state)
 *   |  ----------------------------------       |     (jade accent rule)
 *   |  large multi-line info / statistics       |     (18-20px, white, left)
 *   +------------------------------------------+
 */
static void buildUi() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- Header bar: bitcoin-orange strip with black title ---- */
    ui_header = lv_obj_create(scr);
    lv_obj_remove_style_all(ui_header);
    lv_obj_set_size(ui_header, SCREEN_WIDTH, UI_HEADER_HEIGHT);
    lv_obj_align(ui_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(ui_header, COLOR_BITCOIN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_header, LV_OPA_COVER, LV_PART_MAIN);

    ui_title = lv_label_create(ui_header);
    lv_label_set_text(ui_title, "SerTun32 v" FIRMWARE_VERSION);
    lv_obj_set_style_text_color(ui_title, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_title, UI_TITLE_FONT, LV_PART_MAIN);
    lv_obj_center(ui_title);

    /* ---- Big status text on black; only THIS line is state-coloured ---- */
    ui_status = lv_label_create(scr);
    lv_label_set_text(ui_status, "BOOTING");
    lv_obj_set_style_text_font(ui_status, UI_STATUS_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_status, COLOR_WARN, LV_PART_MAIN);
    lv_obj_align(ui_status, LV_ALIGN_TOP_MID, 0, UI_STATUS_Y + UI_STATUS_EXTRA_Y);

    /* ---- Thin white separator rule under the status ---- */
    ui_rule = lv_obj_create(scr);
    lv_obj_remove_style_all(ui_rule);
    lv_obj_set_size(ui_rule, SCREEN_WIDTH - (UI_INFO_WIDTH_MARGIN * 4), 2);
    lv_obj_align(ui_rule, LV_ALIGN_TOP_MID, 0, UI_RULE_Y);
    lv_obj_set_style_bg_color(ui_rule, COLOR_TXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_rule, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- Info / statistics block ---- */
    ui_info = lv_label_create(scr);
    lv_label_set_long_mode(ui_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui_info, SCREEN_WIDTH - (UI_INFO_WIDTH_MARGIN * 2));
    lv_obj_align(ui_info, LV_ALIGN_TOP_LEFT, UI_INFO_WIDTH_MARGIN, UI_INFO_Y);
    lv_label_set_text(ui_info, "");
    lv_obj_set_style_text_font(ui_info, UI_INFO_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_info, COLOR_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(ui_info, UI_INFO_LINE_SPACE, LV_PART_MAIN);
}

/** Set the big status text (icon included) and its colour. ONLY the status line
 *  is state-coloured (yellow = setup/busy, green = TUNNEL OK, red = error); the
 *  separator rule and info block stay white. */
static void uiSetStatus(const char *text, lv_color_t color) {
    lv_label_set_text(ui_status, text);
    lv_obj_set_style_text_color(ui_status, color, LV_PART_MAIN);
}

/** Set the multi-line info / statistics block, optionally with a bigger font. */
static void uiSetInfo(const String &text, const lv_font_t *font = UI_INFO_FONT) {
    lv_obj_set_style_text_font(ui_info, font, LV_PART_MAIN);
    lv_label_set_text(ui_info, text.c_str());
}

/* ========================================================================= */
/*  Captive-portal web server                                                */
/* ========================================================================= */

/** Minimal HTML-attribute escaping for SSIDs shown in the form. */
static String htmlEscape(const String &in) {
    String out;
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '\'': out += "&#39;";  break;
            case '"':  out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

/**
 * @brief Scan for nearby WiFi networks and cache them as <option> tags.
 *
 * Runs a synchronous scan (a few seconds) while the AP is up; the result
 * populates a <datalist> so the user can pick an SSID instead of typing it.
 * De-duplicates SSIDs and skips hidden (empty) ones.
 */
static void scanWifiNetworks() {
    g_ssidOptions = "";
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        String opt = "<option value='" + htmlEscape(ssid) + "'>";
        if (g_ssidOptions.indexOf(opt) >= 0) continue;   /* de-dupe */
        g_ssidOptions += opt;
    }
    WiFi.scanDelete();
}

/** Render the configuration HTML form (pre-filled with current values). */
static String buildConfigPage() {
    String html =
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SerTun32 Setup</title><style>"
        "body{font-family:sans-serif;background:#000;color:#eee;margin:0;padding:1em}"
        "h1{color:#00b377;font-size:1.4em;border-bottom:2px solid #00b377;padding-bottom:.3em}"
        "form{max-width:420px;margin:auto}"
        "label{display:block;margin:.8em 0 .2em;font-size:.9em;color:#80a99b}"
        "input{width:100%;box-sizing:border-box;padding:.6em;border:1px solid #00b377;"
        "border-radius:6px;background:#0a0f0d;color:#eee;font-size:1em}"
        "button{margin-top:1.2em;width:100%;padding:.8em;border:0;border-radius:6px;"
        "background:#00b377;color:#000;font-size:1.1em;font-weight:bold}"
        ".hint{color:#80a99b;font-size:.8em;margin-top:.2em}"
        "</style></head><body><form method='POST' action='/save'><h1>SerTun32 v"
        FIRMWARE_VERSION " Setup</h1>"
        "<p class='hint'>Configure the WiFi network to join and the TCP socket "
        "to tunnel the serial port to.</p>"
        "<label>WiFi SSID</label>"
        "<input name='ssid' maxlength='32' list='nets' autocomplete='off' "
        "value='" + g_cfg.wifiSsid + "' placeholder='pick or type' required>"
        "<datalist id='nets'>" + g_ssidOptions + "</datalist>"
        "<label>WiFi Password</label>"
        "<input name='pass' type='password' maxlength='64' value='" + g_cfg.wifiPass + "'>"
        "<label>TCP Host (IP address)</label>"
        "<input name='tcpip' maxlength='40' value='" + g_cfg.tcpIp + "' "
        "placeholder='192.168.1.50' required>"
        "<label>TCP Port</label>"
        "<input name='tcpport' type='number' min='1' max='65535' value='" +
        (g_cfg.tcpPort ? String(g_cfg.tcpPort) : String("")) + "' "
        "placeholder='9000' required>"
        "<button type='submit'>Save &amp; Connect</button>"
        "</form></body></html>";
    return html;
}

/** GET / -> serve the configuration form. */
static void handleRoot() {
    httpServer.send(200, "text/html", buildConfigPage());
}

/** POST /save -> validate, persist, and start connecting. */
static void handleSave() {
    if (httpServer.hasArg("ssid"))    g_cfg.wifiSsid = httpServer.arg("ssid");
    if (httpServer.hasArg("pass"))    g_cfg.wifiPass = httpServer.arg("pass");
    if (httpServer.hasArg("tcpip"))   g_cfg.tcpIp    = httpServer.arg("tcpip");
    if (httpServer.hasArg("tcpport")) g_cfg.tcpPort  =
        (uint16_t)httpServer.arg("tcpport").toInt();

    saveConfig();

    String html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title><style>body{font-family:sans-serif;background:#000;"
        "color:#eee;text-align:center;padding:2em}h1{color:#00b377}</style></head>"
        "<body><h1>Saved!</h1><p>SerTun32 is now connecting to <b>" +
        g_cfg.wifiSsid + "</b>.<br>You can close this page.</p></body></html>";
    httpServer.send(200, "text/html", html);

    enterConnectingState();
}

/**
 * @brief Catch-all handler: redirect every unknown host to the portal.
 *
 * This is what makes the OS pop up the "Sign in to network" captive-portal
 * notification on phones / laptops.
 */
static void handleCaptiveRedirect() {
    httpServer.sendHeader("Location",
                          String("http://") + g_apIp.toString(), true);
    httpServer.send(302, "text/plain", "");
}

/* ========================================================================= */
/*  State transitions                                                        */
/* ========================================================================= */

/** Enter STATE_CONFIG: start AP + DNS + HTTP, show credentials on screen. */
static void enterConfigState() {
    g_state = STATE_CONFIG;

    /* Tear down any tunnel / station connection. AP+STA so the station radio
     * is available for scanning nearby networks while the AP serves clients. */
    if (tcpClient.connected()) tcpClient.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);

    WiFi.softAP(g_apSsid.c_str(), AP_PASSWORD);
    g_apIp = WiFi.softAPIP();

    dnsServer.start(AP_DNS_PORT, "*", g_apIp);

    httpServer.onNotFound(handleCaptiveRedirect);
    httpServer.on("/", HTTP_GET, handleRoot);
    httpServer.on("/save", HTTP_POST, handleSave);
    /* URLs probed by various OSes to detect captive portals. */
    httpServer.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
    httpServer.on("/hotspot-detect.html", HTTP_GET, handleRoot);
    httpServer.begin();

    uiSetStatus(LV_SYMBOL_SETTINGS " SETUP", COLOR_WARN);
    String info =
        LV_SYMBOL_WIFI "  " + g_apSsid + "\n"
        LV_SYMBOL_KEYBOARD "  " AP_PASSWORD "\n"
        LV_SYMBOL_HOME "  " + g_apIp.toString();
    uiSetInfo(info, UI_INFO_FONT);

    /* Populate the SSID picker (blocks a few seconds; AP is already up). */
    scanWifiNetworks();
}

/** Enter STATE_CONNECTING: begin joining the configured WiFi network. */
static void enterConnectingState() {
    g_state = STATE_CONNECTING;
    g_connectStartTime = millis();

    /* Stop portal services. */
    dnsServer.stop();
    httpServer.stop();

    WiFi.mode(WIFI_STA);
    WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str());

    uiSetStatus(LV_SYMBOL_REFRESH " CONNECTING", COLOR_WARN);
    uiSetInfo("Joining WiFi\n" LV_SYMBOL_WIFI "  " + g_cfg.wifiSsid,
              UI_INFO_FONT);
}

/** Enter STATE_TUNNEL: WiFi is up, start bridging. */
static void enterTunnelState() {
    g_state = STATE_TUNNEL;

    g_stats = {0, 0, 0, 0};
    g_lastTcpAttempt   = 0;       /* force an immediate first attempt        */
    g_lastStatsUpdate  = 0;
    g_socketWasUp      = false;

    uiSetStatus(LV_SYMBOL_OK " WIFI OK", COLOR_OK);
    uiSetInfo(LV_SYMBOL_WIFI "  " + WiFi.localIP().toString() +
              "\n" LV_SYMBOL_UPLOAD "  " + g_cfg.tcpIp + ":" + String(g_cfg.tcpPort) +
              "\n" LV_SYMBOL_REFRESH "  linking socket...",
              &lv_font_montserrat_18);
}

/* ========================================================================= */
/*  Per-state loop handlers                                                  */
/* ========================================================================= */

/** STATE_CONFIG: service DNS + HTTP so the captive portal stays responsive. */
static void loopConfig() {
    dnsServer.processNextRequest();
    httpServer.handleClient();
}

/** STATE_CONNECTING: poll the WiFi join, advance or time out. */
static void loopConnecting() {
    if (WiFi.status() == WL_CONNECTED) {
        enterTunnelState();
        return;
    }
    if (millis() - g_connectStartTime > WIFI_CONNECT_TIMEOUT_MS) {
        uiSetStatus(LV_SYMBOL_WARNING " WIFI FAIL", COLOR_ERR);
        uiSetInfo("Could not join\n" LV_SYMBOL_WIFI "  " + g_cfg.wifiSsid +
                  "\nReturning to setup...", UI_INFO_FONT);
        delay(1500);
        enterConfigState();
    }
}

/** Format a byte count using binary-scaled B, KB, MB, or GB units. */
static String formatByteCount(uint64_t bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;

    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0) {
        return String(static_cast<unsigned long long>(bytes)) + " B";
    }
    return String(value, 3) + " " + units[unit];
}

/**
 * @brief Refresh the on-screen statistics block (called periodically).
 */
static void updateStatsDisplay(bool socketUp) {
    String info;
    /* Tunnel target socket (IP:port). */
    info  = LV_SYMBOL_SHUFFLE "  " + g_cfg.tcpIp + ":" + String(g_cfg.tcpPort) + "\n";
    info += LV_SYMBOL_UPLOAD "  Ser" LV_SYMBOL_RIGHT "Sock  " +
            String(g_stats.msgsSerialToSocket) +
            "  " + formatByteCount(g_stats.bytesSerialToSocket) + "\n";
    info += LV_SYMBOL_DOWNLOAD "  Sock" LV_SYMBOL_RIGHT "Ser  " +
            String(g_stats.msgsSocketToSerial) +
            "  " + formatByteCount(g_stats.bytesSocketToSerial);
    uiSetInfo(info, UI_INFO_FONT);

    if (socketUp) {
        uiSetStatus(LV_SYMBOL_OK " TUNNEL OK", COLOR_OK);
    } else {
        uiSetStatus(LV_SYMBOL_WARNING " NO SOCKET", COLOR_ERR);
    }
}

/**
 * @brief STATE_TUNNEL: keep the socket up and pump bytes both ways.
 */
static void loopTunnel() {
    /* WiFi dropped? Go re-connect. */
    if (WiFi.status() != WL_CONNECTED) {
        if (tcpClient.connected()) tcpClient.stop();
        enterConnectingState();
        return;
    }

    /* (Re)establish the TCP socket if needed. */
    if (!tcpClient.connected()) {
        if (g_socketWasUp) {
            g_socketWasUp = false;
        }
        uint32_t now = millis();
        if (now - g_lastTcpAttempt >= TCP_RECONNECT_INTERVAL_MS) {
            g_lastTcpAttempt = now;
            if (tcpClient.connect(g_cfg.tcpIp.c_str(), g_cfg.tcpPort)) {
                tcpClient.setNoDelay(true);
                g_socketWasUp = true;
            }
        }
    }

    /* Pump data while the socket is up. Each direction is fully drained, and
     * every write is retried until ALL bytes are out, so nothing is lost. */
    if (tcpClient.connected()) {
        static uint8_t buf[TCP_RX_BUFFER_SIZE];
        int avail;

        /* USB-C serial (host COM port)  ->  TCP socket */
        while ((avail = Serial.available()) > 0) {
            int n = Serial.readBytes(buf, min(avail, (int)sizeof(buf)));
            if (n <= 0) break;
            int sent = 0;
            uint32_t stalledSince = 0;
            while (sent < n && tcpClient.connected()) {
                int w = tcpClient.write(buf + sent, n - sent);
                if (w > 0) {
                    sent += w;
                    stalledSince = 0;
                } else {
                    if (stalledSince == 0) stalledSince = millis();
                    if (millis() - stalledSince >= TCP_WRITE_STALL_TIMEOUT_MS) {
                        tcpClient.stop();
                        break;
                    }
                    delay(1);                 /* socket buffer full, back off */
                }
            }
            g_stats.msgsSerialToSocket++;
            g_stats.bytesSerialToSocket += n;
        }

        /* TCP socket -> USB-C serial. Do not consume TCP data until the host
         * has completed the CDC connection handshake; leaving it in lwIP
         * applies TCP backpressure instead of stranding the first USB write. */
        if (Serial) {
            bool wroteToSerial = false;
            while ((avail = tcpClient.available()) > 0) {
                int n = tcpClient.readBytes(buf, min(avail, (int)sizeof(buf)));
                if (n <= 0) break;
                int sent = 0;
                while (sent < n) {
                    int w = Serial.write(buf + sent, n - sent);
                    if (w > 0) sent += w;
                    else       delay(1);      /* USB TX ring busy, retry */
                }
                g_stats.msgsSocketToSerial++;
                g_stats.bytesSocketToSerial += n;
                wroteToSerial = true;
            }
            /* The guarded flush drains HWCDC's final 64-byte FIFO chunk.
             * Calling flush while disconnected would discard queued data. */
            if (wroteToSerial && Serial) Serial.flush();
        }
    }

    /* Periodic stats refresh. */
    uint32_t now = millis();
    if (now - g_lastStatsUpdate >= STATS_UPDATE_INTERVAL_MS) {
        g_lastStatsUpdate = now;
        updateStatsDisplay(tcpClient.connected());
    }
}

/* ========================================================================= */
/*  Button: return to configuration                                          */
/* ========================================================================= */

/**
 * @brief Detect a debounced press of the config button.
 * @return true exactly once per physical press.
 */
static bool configButtonPressed() {
    static bool     wasPressed   = false;
    static uint32_t pressedSince = 0;

    bool level = (digitalRead(BUTTON_CONFIG_PIN) == BUTTON_ACTIVE_LEVEL);

    if (level && !wasPressed) {
        if (pressedSince == 0) pressedSince = millis();
        if (millis() - pressedSince >= BUTTON_HOLD_MS) {
            wasPressed   = true;
            pressedSince = 0;
            return true;
        }
    } else if (!level) {
        wasPressed   = false;
        pressedSince = 0;
    }
    return false;
}

/* ========================================================================= */
/*  Arduino entry points                                                     */
/* ========================================================================= */

void setup() {
    /* Framework and ESP-IDF diagnostics must never enter the USB data stream. */
    esp_log_level_set("*", ESP_LOG_NONE);

    /* USB-C port (USB-CDC): the ONLY thing on this port is the tunnel data.
     * No debug logging is emitted on any serial port - all status goes to the
     * display - so the stream stays byte-exact.
     * NB: over USB-CDC the baud rate is virtual (the host sets it; the device
     * ignores it). 115200 is configured for tooling that expects a value. */
    Serial.setRxBufferSize(TUNNEL_RX_BUFFER);
    Serial.setTxBufferSize(TUNNEL_TX_BUFFER);
    Serial.begin(TUNNEL_BAUD);              /* TUNNEL_BAUD = 115200           */
    Serial.setTimeout(5);                   /* don't block in readBytes()     */

    pinMode(BUTTON_CONFIG_PIN, INPUT_PULLUP);

    /* ---- Board power-enable (T-Display-S3: GPIO15 powers the LCD) ---- */
#ifdef BOARD_POWER_ON_PIN
    pinMode(BOARD_POWER_ON_PIN, OUTPUT);
    digitalWrite(BOARD_POWER_ON_PIN, HIGH);
#endif

    /* ---- Display init ---- */
    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
#ifdef TFT_BL
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif

    /* ---- LVGL init ---- */
    lv_init();
    lv_disp_draw_buf_init(&s_drawBuf, s_lvBuf, nullptr,
                          SCREEN_WIDTH * LV_DRAW_BUF_LINES);

    static lv_disp_drv_t dispDrv;
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res  = SCREEN_WIDTH;
    dispDrv.ver_res  = SCREEN_HEIGHT;
    dispDrv.flush_cb = lvDispFlush;
    dispDrv.draw_buf = &s_drawBuf;
    lv_disp_drv_register(&dispDrv);

    buildUi();

    /* ---- Derive a per-device AP SSID from the MAC ---- */
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char suffix[5];
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    g_apSsid = String(AP_SSID_PREFIX) + suffix;

    /* ---- Load config and pick the initial state ---- */
    loadConfig();

    if (configIsComplete()) {
        enterConnectingState();   /* we already know where to go             */
    } else {
        enterConfigState();       /* first boot / incomplete config          */
    }
}

void loop() {
    /* Run the state handler FIRST and as often as possible. In STATE_TUNNEL the
     * USB-CDC RX ring must be drained faster than the host fills it, otherwise
     * it overflows and corrupts the stream - so the pump must never wait behind
     * the (relatively slow) display rendering. */
    switch (g_state) {
        case STATE_CONFIG:     loopConfig();     break;
        case STATE_CONNECTING: loopConnecting(); break;
        case STATE_TUNNEL:     loopTunnel();     break;
    }

    /* Button -> back to configuration (only meaningful once provisioned). */
    if (g_state != STATE_CONFIG && configButtonPressed()) {
        enterConfigState();
    }

    /* Drive LVGL on a throttled cadence (~30 fps) so a redraw can never stall
     * the tunnel pump. lv_tick is fed from millis() (see lv_conf.h). */
    static uint32_t lastLv = 0;
    uint32_t now = millis();
    if (now - lastLv >= 33) {
        lastLv = now;
        lv_timer_handler();
    }

    /* Yield to the WiFi / USB FreeRTOS tasks without throttling throughput:
     * delay(0) feeds the watchdog and reschedules but adds no fixed latency. */
    delay(0);
}

#endif
