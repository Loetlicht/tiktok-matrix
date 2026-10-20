# Firmware: matrix_tiktok

Arduino-Sketch für ESP32 WROOM und eine 32×8-LED-Matrix mit 4× MAX7219 (FC16-Module).

## Einrichtung

1. **Board:** Arduino IDE → Boardverwalter → *esp32 von Espressif* installieren, Board **ESP32 Dev Module** wählen.
2. **Bibliothek:** Bibliotheksverwalter → **MD_Parola** (von majicDesigns) installieren. MD_MAX72XX kommt automatisch mit.
3. **WLAN:** `secrets_example.h` in `secrets.h` umbenennen (in der IDE: Tab → *Umbenennen*) und SSID und Passwort eintragen. Der ESP32 braucht ein **2,4-GHz-WLAN**.
   `secrets.h` steht in der `.gitignore` und landet nie auf GitHub.
4. **TikTok-Name:** In `matrix_tiktok.ino` bei `TIKTOK_USER` deinen Namen ohne `@` eintragen.
5. Hochladen.

## Einstellungen oben im Sketch

| Konstante | Standard | Bedeutung |
|---|---|---|
| `HARDWARE_TYPE` | `FC16_HW` | bei gespiegelter oder verdrehter Schrift `GENERIC_HW` oder `PAROLA_HW` probieren |
| `PIN_DIN / PIN_CLK / PIN_CS` | 23 / 18 / 5 | Anschlüsse am ESP32 |
| `ZEIT_DAUER_MS` | 15000 | wie lange die Uhrzeit steht |
| `FOLLOWER_DAUER_MS` | 6000 | wie lange die Follower stehen |
| `LAUFTEXT_SPEED` | 40 | kleiner = schnellerer Lauftext |
| `FOLLOWER_INTERVALL` | 10 min | wie oft TikTok abgefragt wird |
| `HELL_TAG / HELL_NACHT` | 4 / 0 | Helligkeit 0 bis 15 |
| `NACHT_VON / NACHT_BIS` | 22 / 7 | Nachtmodus in Stunden |

## Serieller Monitor

Der Sketch sendet mit **9600 Baud**. Nach jedem Abruf steht dort zum Beispiel:

```
TikTok HTTP 200, Laenge laut Server: -1
TikTok gelesen: 283117 Bytes in 6200 ms
  captcha         0
  ...
TikTok Follower: 1234
```

## Fehlersuche

| Anzeige / Ausgabe | Ursache | Lösung |
|---|---|---|
| `fatal error: secrets.h` beim Kompilieren | Datei heisst noch `secrets_example.h` | umbenennen, siehe oben |
| Zeichensalat im seriellen Monitor | Baudrate falsch | im Monitor rechts unten 9600 Baud einstellen |
| `♪ --` auf der Matrix | noch keine Follower-Zahl erhalten | seriellen Monitor ansehen |
| `captcha` > 0 und `Follower: -1` | TikTok blockt dieses Netz, oft in Firmen-WLANs | anderes WLAN, z. B. zuhause; die letzte Zahl bleibt stehen |
| Schrift gespiegelt oder Blöcke vertauscht | anderer Modultyp | `HARDWARE_TYPE` ändern |
