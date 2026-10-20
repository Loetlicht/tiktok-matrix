/*
  Lötlicht LED-Matrix-Pult
  ESP32 WROOM + 32x8 LED-Matrix (4x MAX7219, FC16-Module)

  Anzeige im Wechsel:
    1. Uhrzeit  (hh:mm, blinkender Doppelpunkt)
    2. Datum + Kalenderwoche (Lauftext, z.B. "So 18.10.2026  KW 42")
    3. TikTok-Follower von @loetlicht (Noten-Symbol + Zahl)

  Bibliotheken (Arduino IDE -> Bibliotheksverwalter):
    - MD_Parola    (majicDesigns)
    - MD_MAX72XX   (majicDesigns)
  Board: "ESP32 Dev Module"

  Verdrahtung Matrix -> ESP32:
    VCC -> 5V (VIN)   GND -> GND
    DIN -> GPIO 23    CLK -> GPIO 18    CS -> GPIO 5

  WLAN-Daten: secrets_example.h nach secrets.h kopieren und ausfüllen.

  Lizenz: CC BY-NC-SA 4.0 – Lötlicht
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <time.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS

// ---------------- Einstellungen ----------------
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW   // bei Zeichensalat: GENERIC_HW oder PAROLA_HW probieren
#define MAX_DEVICES   4
#define PIN_DIN       23
#define PIN_CLK       18
#define PIN_CS        5

const char* TIKTOK_USER   = "loetlicht";
const char* TZ_ZURICH     = "CET-1CEST,M3.5.0,M10.5.0/3";   // Sommer-/Winterzeit automatisch
const char* NTP_SERVER    = "ch.pool.ntp.org";

const uint32_t ZEIT_DAUER_MS      = 15000;   // wie lange die Uhrzeit steht
const uint32_t FOLLOWER_DAUER_MS  = 6000;    // wie lange die Follower stehen
const uint8_t  LAUFTEXT_SPEED     = 40;      // kleiner = schneller
const uint32_t FOLLOWER_INTERVALL = 10UL * 60UL * 1000UL;  // alle 10 Minuten abfragen

const uint8_t HELL_TAG   = 4;   // 0..15
const uint8_t HELL_NACHT = 0;
const uint8_t NACHT_VON  = 22;  // Uhr
const uint8_t NACHT_BIS  = 7;

// ---------------- Globale Objekte ----------------
MD_Parola P = MD_Parola(HARDWARE_TYPE, PIN_DIN, PIN_CLK, PIN_CS, MAX_DEVICES);

// TikTok-Note als eigenes Zeichen (ersetzt '~'), Spalten von links, Bit0 = oben
uint8_t noteGlyph[] = { 6, 0x60, 0x70, 0x70, 0x3F, 0x03, 0x06 };

enum Modus { M_ZEIT, M_DATUM, M_FOLLOWER };
Modus modus = M_ZEIT;
uint32_t modusStart = 0;

long followers = -1;          // -1 = noch unbekannt
uint32_t letzterAbruf = 0;
bool ersterAbruf = true;

char anzeige[48];

const char* WOCHENTAG[] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };

// ---------------- Hilfsfunktionen ----------------

// ISO-8601-Kalenderwoche
// Die KW ist die Woche, in der der Donnerstag liegt.
static int tageImJahr(int y) {
  return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;
}
int kalenderwoche(const struct tm& t) {
  int wday = (t.tm_wday + 6) % 7;                 // Mo=0 .. So=6
  int donnerstag = t.tm_yday - wday + 3;          // Tag im Jahr des Donnerstags dieser Woche
  int jahr = t.tm_year + 1900;
  if (donnerstag < 0)                             // Donnerstag liegt im Vorjahr
    donnerstag += tageImJahr(jahr - 1);
  else if (donnerstag >= tageImJahr(jahr))        // Donnerstag liegt im Folgejahr
    donnerstag -= tageImJahr(jahr);
  return donnerstag / 7 + 1;
}

// 1234 -> "1234", 12345 -> "12.3k", 1234567 -> "1.23M"
void formatZahl(long n, char* out, size_t len) {
  if (n < 0)            snprintf(out, len, "--");
  else if (n < 10000)   snprintf(out, len, "%ld", n);
  else if (n < 1000000) snprintf(out, len, "%.1fk", n / 1000.0);
  else                  snprintf(out, len, "%.2fM", n / 1000000.0);
}

// Liest die öffentliche Profilseite und sucht "followerCount":12345
// TikTok kann das Format jederzeit ändern – dann bleibt der letzte Wert stehen.
long holeFollower() {
  WiFiClientSecure client;
  client.setInsecure();                 // kein Zertifikat hinterlegt
  client.setTimeout(15);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  String url = String("https://www.tiktok.com/@") + TIKTOK_USER;
  if (!http.begin(client, url)) return -1;

  http.addHeader("User-Agent",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/129.0 Safari/537.36");
  http.addHeader("Accept-Language", "de-CH,de;q=0.9");
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("TikTok HTTP %d\n", code);
    http.end();
    return -1;
  }

  Serial.printf("TikTok HTTP %d, Laenge laut Server: %d\n", code, http.getSize());

  // Seite im Strom durchsuchen, ohne sie ganz in den RAM zu laden.
  // Gesucht wird followerCount, danach werden Zeichen wie " \ : übersprungen.
  const char* muster = "followerCount";
  const size_t mlen = strlen(muster);
  size_t treffer = 0;
  long wert = -1;
  int nachMuster = -1;                  // Zeichen seit Treffer, -1 = noch kein Treffer

  // Diagnose: Anfang der Seite merken, Stichworte zählen
  String anfang;
  uint32_t bytes = 0;
  struct Stichwort { const char* wort; size_t pos; int anzahl; };
  Stichwort sw[] = { {"captcha", 0, 0}, {"verify", 0, 0}, {"UNIVERSAL_DATA", 0, 0},
                     {"userInfo", 0, 0}, {"statusCode", 0, 0} };
  const int SW_N = sizeof(sw) / sizeof(sw[0]);

  WiFiClient* s = http.getStreamPtr();
  uint32_t t0 = millis();

  while ((http.connected() || s->available()) && millis() - t0 < 25000) {
    if (!s->available()) { delay(2); continue; }
    char c = s->read();
    bytes++;
    if (anfang.length() < 300 && c >= 32 && c < 127) anfang += c;

    for (int i = 0; i < SW_N; i++) {
      size_t L = strlen(sw[i].wort);
      if (tolower(c) == tolower(sw[i].wort[sw[i].pos])) {
        if (++sw[i].pos == L) { sw[i].anzahl++; sw[i].pos = 0; }
      } else {
        sw[i].pos = (tolower(c) == tolower(sw[i].wort[0])) ? 1 : 0;
      }
    }

    if (nachMuster < 0) {
      treffer = (c == muster[treffer]) ? treffer + 1 : (c == muster[0] ? 1 : 0);
      if (treffer == mlen) nachMuster = 0;
    } else {
      nachMuster++;
      if (isdigit(c)) {
        wert = (wert < 0 ? 0 : wert) * 10 + (c - '0');
      } else if (wert >= 0) {
        break;                          // Zahl vollständig
      } else if (nachMuster > 6) {      // keine Zahl direkt dahinter: weitersuchen
        nachMuster = -1; treffer = 0;
      }
    }
  }
  http.end();

  Serial.printf("TikTok gelesen: %u Bytes in %u ms\n", bytes, millis() - t0);
  for (int i = 0; i < SW_N; i++) Serial.printf("  %-15s %d\n", sw[i].wort, sw[i].anzahl);
  Serial.println("  Seitenanfang: " + anfang);
  Serial.printf("TikTok Follower: %ld\n", wert);
  return wert;
}

void setzeHelligkeit(const struct tm& t) {
  bool nacht = (t.tm_hour >= NACHT_VON) || (t.tm_hour < NACHT_BIS);
  P.setIntensity(nacht ? HELL_NACHT : HELL_TAG);
}

void zeigeStatisch(const char* txt) {
  P.displayText(txt, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  P.displayAnimate();
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(9600);

  P.begin();
  P.setIntensity(HELL_TAG);
  P.addChar('~', noteGlyph);
  P.setCharSpacing(1);
  zeigeStatisch("WLAN");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "WLAN fehlgeschlagen");

  configTzTime(TZ_ZURICH, NTP_SERVER, "pool.ntp.org");
  zeigeStatisch("Zeit..");
  struct tm t;
  getLocalTime(&t, 10000);

  modusStart = millis();
}

// ---------------- Loop ----------------
void loop() {
  // WLAN bei Abbruch neu verbinden
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

  // Follower regelmässig holen (nicht während Lauftext, damit nichts ruckelt)
  if (modus != M_DATUM && WiFi.status() == WL_CONNECTED &&
      (ersterAbruf || millis() - letzterAbruf > FOLLOWER_INTERVALL)) {
    long f = holeFollower();
    if (f >= 0) followers = f;
    letzterAbruf = millis();
    ersterAbruf = false;
  }

  struct tm t;
  bool zeitOk = getLocalTime(&t, 50);
  if (zeitOk) setzeHelligkeit(t);

  switch (modus) {

    case M_ZEIT: {
      static int letzteSek = -1;
      if (zeitOk && t.tm_sec != letzteSek) {
        letzteSek = t.tm_sec;
        snprintf(anzeige, sizeof(anzeige), (t.tm_sec % 2) ? "%02d:%02d" : "%02d %02d",
                 t.tm_hour, t.tm_min);
        zeigeStatisch(anzeige);
      }
      if (millis() - modusStart > ZEIT_DAUER_MS) {
        if (zeitOk) {
          snprintf(anzeige, sizeof(anzeige), "%s %02d.%02d.%04d  KW %d",
                   WOCHENTAG[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
                   kalenderwoche(t));
        } else {
          snprintf(anzeige, sizeof(anzeige), "Keine Zeit");
        }
        P.displayClear();
        P.displayText(anzeige, PA_LEFT, LAUFTEXT_SPEED, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
        modus = M_DATUM;
      }
      break;
    }

    case M_DATUM:
      if (P.displayAnimate()) {          // Lauftext fertig
        char zahl[12];
        formatZahl(followers, zahl, sizeof(zahl));
        snprintf(anzeige, sizeof(anzeige), "~ %s", zahl);
        zeigeStatisch(anzeige);
        modus = M_FOLLOWER;
        modusStart = millis();
      }
      break;

    case M_FOLLOWER:
      if (millis() - modusStart > FOLLOWER_DAUER_MS) {
        modus = M_ZEIT;
        modusStart = millis();
      }
      break;
  }
}
