# G4MEOVER WiFi-Relay (ESP32-WROOM)

Der **WROOM als WiFi-Koordinator** des G4MEOVER-Ökosystems. Nimmt die signierten
`ukfe_rf`-Befehle des Flipper über **GPIO-UART** entgegen, verifiziert sie
(keyed MAC + CRC16 + Rolling-Counter) und **rebroadcastet** sie per **ESP-NOW**
an die Satelliten. Antworten der Satelliten reicht er zurück an den Flipper.

```
Flipper Zero ──GPIO-UART (ukfe_rf)──► WROOM (Relay) ──ESP-NOW──► LilyGo T-Dongle S3
   (RF-Console lora_ukfe)                  │  validiert            Heltec LoRa v3
                                           └─ Antworten ◄──────────  (…und weitere)
```

**Ein Vokabular über zwei Transporte:** Das Frame-Format ist bitgleich zum
868-FSK-Funklink (`heltec-ukfe-rx`). Ein Satellit validiert Funk- **und**
WiFi-Frames mit demselben `ukfe_rf.c`. Der WROOM ergänzt den 868-Kanal um einen
2,4-GHz-Kanal mit mehr Bandbreite und Reichweite in der Fläche.

## Die zwei Rollen des WROOM
| Rolle | Beschreibung | Status |
|---|---|---|
| **A — Standalone-Penetrator** (microUSB am PC) | USB/WiFi-Angriffsplattform | eigenes Repo **`usb-army-penetrator`** (USB Army Knife Fork) |
| **B — WiFi-Sender/Koordinator** | Flipper→WROOM→Satelliten (dieses Repo) | Firmware kompiliert; Enden-Integration offen |

> **Hardware-Ehrlichkeit:** Der klassische ESP32-WROOM hat **kein natives USB**
> (microUSB → CH9102 → UART0), kann also selbst **keine USB-HID-Tastatur**
> emulieren. Die HID-Injektion leisten die S3-Satelliten (Heltec S3, LilyGo S3).
> Rolle A des WROOM ist WiFi-/Serial-getrieben, nicht BadUSB-HID.

## RF-/Link-Konfiguration
| Parameter | Wert |
|---|---|
| ESP-NOW-Kanal | 1 (`RELAY_ESPNOW_CHANNEL`, auf allen Geräten gleich) |
| Ziel | Broadcast `FF:FF:FF:FF:FF:FF` (alle Satelliten) |
| Flipper-UART | Serial2, RX=GPIO16 ← Flipper-TX(Pin13), TX=GPIO17 → Flipper-RX(Pin14), 115200 8N1 |
| Frame | `ukfe_rf` `[LEN][MAGIC][VER][COUNTER(4)][CMD][ALEN][ARGS][MAC(4)][CRC16]` |
| Secret | 16 B, identisch mit `RF_SECRET`/`UKFE_SECRET` (out-of-band pairen) |

> Die UART-Pins hängen von der Devboard-Verdrahtung ab. Nutzt das Board Flippers
> Pins über **UART0** (wie viele Marauder-Boards), kollidiert das mit
> USB-Flash/Serial — dann entweder auf Serial2-Pins umverdrahten oder Flash nur
> bei vom Flipper **getrenntem** Board (siehe unten).

## Bauen & Flashen
```
pio run                                          # bauen
pio run -t upload --upload-port COM8             # flashen
pio device monitor -p COM8 -b 115200             # CLI/Log
```
> **Flash-Voraussetzung:** Board **vom Flipper-GPIO trennen** und einzeln per USB
> anschließen. Steckt es am Flipper, belegt dessen UART die Programmierleitung
> (esptool: „TX path seems to be down").

## Serial-CLI (Diagnose / Standalone-Test)
- `status` — MAC, Kanal, Zähler (rx/relayed/rejected/resp)
- `ping` — sendet selbst einen STATUS-Frame per ESP-NOW an die Satelliten
- `hex 0E 47 01 …` — Frame einspeisen, als käme er vom Flipper (validieren + relayen)
- `help`

## Offene Integration (die zwei Enden)
1. **Satelliten-RX:** ESP-NOW-Empfangspfad in `heltec-ukfe-rx` (und LilyGo-FW)
   ergänzen — ruft dieselben Handler wie der 868-RX (gemeinsames `ukfe_rf.c`).
2. **Flipper-TX über UART:** `lora-ukfe` um einen UART-Sendepfad erweitern
   (parallel zum CC1101-Funk), damit „RF-Console" auch über den WROOM geht.

> Nur für autorisierte Sicherheitstests auf eigenen Geräten.
