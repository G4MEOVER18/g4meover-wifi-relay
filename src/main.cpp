// G4MEOVER WiFi-Relay — ESP32-WROOM als ESP-NOW-Koordinator
// ---------------------------------------------------------------------------
// Rolle B des G4MEOVER-Oekosystems: nimmt die signierten ukfe_rf-Befehle des
// Flipper (ueber GPIO-UART) entgegen, verifiziert sie (keyed MAC + CRC16 +
// Rolling-Counter) und rebroadcastet sie per ESP-NOW an die Satelliten
// (LilyGo T-Dongle S3, Heltec LoRa v3). Antworten der Satelliten werden
// zurueck an den Flipper-UART gereicht. Ein Vokabular (ukfe_rf) ueber beide
// Transporte — identisch zum 868-FSK-Funklink.
//
// Physikalische Schicht hier = WiFi/ESP-NOW (2.4 GHz) statt 868-FSK; das
// Frame-Format (ukfe_rf) bleibt Bit-fuer-Bit gleich, daher validieren die
// Satelliten Funk- UND WiFi-Frames mit demselben Code.
//
// Nur fuer autorisierte Sicherheitstests auf eigenen Geraeten.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

extern "C" {
#include "ukfe_rf.h"
}

// Rolling-Counter über Reboots persistieren (NVS) — sonst desynct der WROOM nach
// Neustart unter das Anti-Replay-Fenster der Satelliten und alle Frames werden
// als Replay verworfen. Margin-Sprung pro Boot statt Schreiben je Ping (NVS-Schonung).
static Preferences g_prefs;
#define TXCTR_MARGIN 100u

// ---- Shared Secret: IDENTISCH mit RF_SECRET (Flipper) und UKFE_SECRET (Heltec) ----
// Out-of-band pairen; Pairing-Bytes vor Produktivnutzung ersetzen.
static const uint8_t RELAY_SECRET[UKFE_RF_SECRET_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,   // "G4MEOVER"
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,   // Pairing-Bytes (ersetzen!)
};

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- Zustand ----
static uint32_t s_rxCounter = 0;     // Anti-Replay-Fenster (Flipper -> Relay)
static uint32_t s_txCounter = 0;     // eigener Counter fuer CLI-generierte Frames
static uint32_t s_rxFromFlipper = 0; // Statistik
static uint32_t s_relayed = 0;
static uint32_t s_rejected = 0;
static uint32_t s_fromSatellite = 0;

// UART-Framing-Akkumulator (Flipper-Link)
static uint8_t  s_buf[128];
static size_t   s_len = 0;

#define LED_PIN 2  // WROOM Onboard-LED

// ---------------------------------------------------------------------------
// ESP-NOW: Frame an alle Satelliten senden
static bool espnow_broadcast(const uint8_t* frame, size_t len) {
    esp_err_t r = esp_now_send(BROADCAST_MAC, frame, len);
    return r == ESP_OK;
}

// ESP-NOW-Empfang (Antworten der Satelliten) -> an Flipper-UART weiterreichen
static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len <= 0 || len > (int)UKFE_RF_MAX_FRAME) return;
    UkfeRfMessage msg;
    // Antworten ohne Replay-Check parsen (NULL) — nur Integritaet zaehlt.
    if (ukfe_rf_parse_frame(RELAY_SECRET, data, (size_t)data[0] + 1, &msg, NULL)) {
        s_fromSatellite++;
        Serial2.write(data, len);   // zum Flipper zurueck
        Serial.printf("[<-SAT] %02X:%02X:%02X:%02X:%02X:%02X resp=0x%02X -> Flipper\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], msg.cmd);
    }
}

// ---------------------------------------------------------------------------
// Einen validierten Frame verarbeiten: an Satelliten rebroadcasten
static void relay_frame(const uint8_t* frame, size_t real_len, const UkfeRfMessage* msg,
                        const char* src) {
    digitalWrite(LED_PIN, HIGH);
    bool ok = espnow_broadcast(frame, real_len);
    if (ok) s_relayed++;
    Serial.printf("[%s] cmd=0x%02X counter=%lu args=%u -> ESP-NOW %s\n",
                  src, msg->cmd, (unsigned long)msg->counter, msg->arg_len,
                  ok ? "OK" : "FAIL");
    digitalWrite(LED_PIN, LOW);
}

// Akkumulator nach einem gueltigen ukfe_rf-Frame durchsuchen.
// Frame-Start = [LEN][MAGIC=0x47][VER=0x01]; echte Laenge = LEN+1.
static void scan_and_relay() {
    while (s_len >= UKFE_RF_HDR_OVERHEAD) {
        // Suche einen plausiblen Frame-Kopf im Puffer.
        size_t i = 0;
        bool found = false;
        for (; i + 2 < s_len; i++) {
            uint8_t L = s_buf[i];
            size_t real_len = (size_t)L + 1;
            if (real_len < UKFE_RF_HDR_OVERHEAD || real_len > UKFE_RF_MAX_FRAME) continue;
            if (s_buf[i + 1] != UKFE_RF_MAGIC || s_buf[i + 2] != UKFE_RF_VERSION) continue;
            found = true;
            break;
        }
        if (!found) {
            // Nichts Plausibles: die letzten 2 Bytes als moeglichen Kopfanfang behalten.
            if (s_len > 2) { memmove(s_buf, s_buf + (s_len - 2), 2); s_len = 2; }
            return;
        }
        // Kopf bei i gefunden — Muell davor verwerfen.
        if (i > 0) { memmove(s_buf, s_buf + i, s_len - i); s_len -= i; }

        size_t real_len = (size_t)s_buf[0] + 1;
        if (s_len < real_len) return;  // Rest des Frames noch nicht da -> warten

        UkfeRfMessage msg;
        if (ukfe_rf_parse_frame(RELAY_SECRET, s_buf, real_len, &msg, &s_rxCounter)) {
            s_rxFromFlipper++;
            relay_frame(s_buf, real_len, &msg, "Flipper");
        } else {
            s_rejected++;
            Serial.println("[Flipper] PARSE FAIL (MAC/CRC/Counter) — verworfen");
        }
        // Frame (oder verworfenen Kandidaten) konsumieren.
        memmove(s_buf, s_buf + real_len, s_len - real_len);
        s_len -= real_len;
    }
}

// ---------------------------------------------------------------------------
// CLI ueber USB-Serial (Diagnose / Standalone-Test)
static void cli_ping() {
    UkfeRfMessage m;
    ukfe_rf_make_simple(&m, UkfeRfCmdStatus);
    m.counter = ++s_txCounter;
    uint8_t frame[UKFE_RF_MAX_FRAME];
    size_t n = ukfe_rf_build_frame(RELAY_SECRET, &m, frame, sizeof(frame));
    if (n && espnow_broadcast(frame, n))
        Serial.printf("[CLI] STATUS-Ping (counter=%lu) an Satelliten gesendet\n",
                      (unsigned long)s_txCounter);
    else
        Serial.println("[CLI] Ping FEHLGESCHLAGEN");
}

static void cli_inject_hex(const String& hex) {
    // "hex 0E 47 01 .." -> Frame in den Flipper-Akkumulator einspeisen
    size_t start = s_len;
    for (int p = 0; p < (int)hex.length();) {
        while (p < (int)hex.length() && hex[p] == ' ') p++;
        if (p + 1 >= (int)hex.length() + 1) break;
        char b[3] = {0, 0, 0};
        int k = 0;
        while (p < (int)hex.length() && hex[p] != ' ' && k < 2) b[k++] = hex[p++];
        if (k == 0) break;
        if (s_len < sizeof(s_buf)) s_buf[s_len++] = (uint8_t)strtol(b, NULL, 16);
    }
    Serial.printf("[CLI] %u Hex-Bytes injiziert\n", (unsigned)(s_len - start));
    scan_and_relay();
}

static void cli_status() {
    Serial.println("=== G4MEOVER WiFi-Relay ===");
    Serial.printf("MAC(STA)     : %s\n", WiFi.macAddress().c_str());
    Serial.printf("ESP-NOW-Kanal: %d\n", RELAY_ESPNOW_CHANNEL);
    Serial.printf("Flipper-UART : RX=%d TX=%d @%d\n", FLIPPER_UART_RX, FLIPPER_UART_TX, FLIPPER_UART_BAUD);
    Serial.printf("rx<-Flipper  : %lu\n", (unsigned long)s_rxFromFlipper);
    Serial.printf("relayed      : %lu\n", (unsigned long)s_relayed);
    Serial.printf("rejected     : %lu\n", (unsigned long)s_rejected);
    Serial.printf("resp<-Sat    : %lu\n", (unsigned long)s_fromSatellite);
}

// Liest UART0 (Serial): dient GLEICHZEITIG als CLI (am USB) UND als zweiter
// Flipper-Frame-Eingang. Grund: Flipper-ESP-Devboards routen Pin 13/14 je nach
// Board auf UART0 (GPIO1/3) ODER UART2 (GPIO16/17) — der Relay akzeptiert beides.
// Physisch exklusiv (USB-CLI XOR Flipper-GPIO), daher kein echter Konflikt.
static void handle_cli() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        // 1) Byte auch in den Frame-Akkumulator (falls Flipper an UART0 haengt).
        //    CLI-Text (z.B. "ping\n") kann keinen Frame vortaeuschen: der Scanner
        //    verlangt [LEN][0x47][0x01] — 0x01 kommt in ASCII-Text nicht vor.
        if (s_len < sizeof(s_buf)) s_buf[s_len++] = (uint8_t)c;
        // 2) CLI-Zeilenlogik
        if (c == '\n' || c == '\r') {
            line.trim();
            if (line == "status") cli_status();
            else if (line == "ping") cli_ping();
            else if (line == "help" || line == "?")
                Serial.println("Befehle: status | ping | hex <bytes> | help");
            else if (line.startsWith("hex ")) cli_inject_hex(line.substring(4));
            else if (line.length()) Serial.println("? 'help' fuer Befehle");
            line = "";
        } else if (line.length() < 200) {
            line += c;
        }
    }
}

// ---------------------------------------------------------------------------
void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.begin(115200);           // USB-CDC/UART0: CLI + Log
    Serial2.begin(FLIPPER_UART_BAUD, SERIAL_8N1, FLIPPER_UART_RX, FLIPPER_UART_TX);

    // TX-Counter aus NVS laden + Margin überspringen (monoton über Reboots).
    g_prefs.begin("g4meover", false);
    s_txCounter = g_prefs.getUInt("txctr", 0) + TXCTR_MARGIN;
    g_prefs.putUInt("txctr", s_txCounter);
    g_prefs.end();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(RELAY_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FEHLGESCHLAGEN — Neustart");
        delay(1500);
        ESP.restart();
    }
    esp_now_register_recv_cb(on_espnow_recv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = RELAY_ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println();
    Serial.println("G4MEOVER WiFi-Relay bereit.");
    Serial.printf("STA-MAC %s | ESP-NOW-Kanal %d | Flipper-UART RX=%d TX=%d\n",
                  WiFi.macAddress().c_str(), RELAY_ESPNOW_CHANNEL,
                  FLIPPER_UART_RX, FLIPPER_UART_TX);
    Serial.println("Warte auf ukfe_rf-Frames vom Flipper... ('help' fuer CLI)");
}

void loop() {
    // Flipper-Frames von UART2 (GPIO16/17) einlesen ...
    while (Serial2.available() && s_len < sizeof(s_buf)) {
        s_buf[s_len++] = (uint8_t)Serial2.read();
    }
    // ... und von UART0 (via handle_cli, deckt Devboards mit Flipper an UART0 ab).
    handle_cli();
    // Akkumulator beider UARTs nach vollstaendigen ukfe_rf-Frames durchsuchen.
    scan_and_relay();
    delay(2);
}
