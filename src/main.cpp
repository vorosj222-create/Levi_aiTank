// Lánctalpas robot – ESP8266
// 1. lépés: WiFi és OTA (WiFi hibakereső kiírásokkal)
// 2. lépés: servo pásztázás
// 3. lépés: ultrahangos mérés pásztázás közben, adatok UDP-n a PC-nek (radarkép)
//
// Első feltöltés USB-n (env: robot), utána WiFi-n (env: robot_ota).

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <Servo.h>
#include <WiFiUdp.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Hianyzik az include/secrets.h! Masoljatok le az include/secrets.h.example fajlt secrets.h neven, es toltsetek ki."
#endif

// ---------- Servo ----------
const uint8_t  PIN_SERVO     = D5;
const int      SWEEP_MIN     = 0;     // fok – ha a servo a szélén zúg/akad, szűkítsétek (pl. 10)
const int      SWEEP_MAX     = 180;   // fok – pl. 170
const uint32_t SWEEP_TIME_MS = 1000;  // ennyi idő alatt ér egyik széltől a másikig
const int      SERVO_MIN_US  = 800;   // impulzushossz 0°-nál (kimérve)
const int      SERVO_MAX_US  = 2200;  // impulzushossz 180°-nál (kimérve)

Servo servo;
bool servoEnabled = true;
int lastServoAngle = -1;

// ---------- Ultrahang ----------
const uint8_t  PIN_TRIG            = D6;
const uint8_t  PIN_ECHO            = D7;    // 5V-os HC-SR04 esetén feszültségosztón keresztül!
const uint32_t ECHO_TIMEOUT_US     = 15000; // kb. 2,5 m – ennél messzebbre nem mér
const uint32_t MEASURE_INTERVAL_MS = 40;    // két mérés között legalább ~30-40 ms kell

uint32_t lastMeasureMs = 0;
uint32_t lastDebugMs = 0;
int lastDistance = -1;

// ---------- UDP (radaradatok a PC-nek) ----------
// ESP -> PC: "R <szog> <tavolsag_cm>"   (-1 = nincs visszhang)
// PC -> ESP: "HELLO" (ebből tudja az ESP a PC címét), válasz: "HELLO OK"
const uint16_t UDP_PORT = 4210;
WiFiUDP udp;
IPAddress pcIP;
uint16_t pcPort = 0;
bool havePC = false;

const uint32_t WIFI_TIMEOUT_MS = 20000;  // ennyi után újrapróbálja
uint32_t lastStatusMs = 0;

// Teszt kapcsolók – egyszerre csak egyet változtassatok!
const bool FORCE_11G = false;   // true: 802.11g mód (Wi-Fi 6 / mesh routereknél segíthet)

WiFiEventHandler disconnectedHandler;  // életben kell tartani, ezért globális
WiFiEventHandler connectedHandler;
WiFiEventHandler gotIpHandler;
volatile bool associated = false;
uint8_t bestBssid[6];
int32_t bestChannel = 0;
bool haveBest = false;

const char* reasonText(uint8_t r) {
  switch (r) {
    case 2:   return "hitelesites lejart (gyakran WPA3/PMF vagy gyenge jel)";
    case 15:
    case 204: return "kezfogas idotullepes -> szinte mindig HIBAS JELSZO (vagy WPA3)";
    case 23:  return "802.1X hitelesites sikertelen";
    case 200: return "beacon idotullepes (gyenge jel)";
    case 201: return "nem talalja a halozatot";
    case 202: return "hitelesites sikertelen (jelszo vagy titkositasi mod)";
    case 203: return "tarsitas sikertelen (MAC szuro, PMF kotelezo, tul sok eszkoz)";
    default:  return "egyeb";
  }
}

const char* statusText(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "varakozik";
    case WL_NO_SSID_AVAIL:   return "NEM LATJA a halozatot (nev elirva, 5 GHz-es, vagy tul messze)";
    case WL_CONNECTED:       return "csatlakozva";
    case WL_CONNECT_FAILED:  return "csatlakozas sikertelen";
    case WL_WRONG_PASSWORD:  return "HIBAS JELSZO";
    case WL_DISCONNECTED:    return "probalkozik...";
    default:                 return "egyeb";
  }
}

const char* encText(uint8_t e) {
  switch (e) {
    case ENC_TYPE_NONE: return "nyitott";
    case ENC_TYPE_WEP:  return "WEP";
    case ENC_TYPE_TKIP: return "WPA";
    case ENC_TYPE_CCMP: return "WPA2";
    case ENC_TYPE_AUTO: return "WPA/WPA2";
    default:            return "ismeretlen (lehet WPA3?)";
  }
}

void scanNetworks() {
  Serial.println("Lathato halozatok keresese...");
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = (WiFi.SSID(i) == WIFI_SSID);
    if (match) {
      found = true;
      static int32_t bestRssi = -1000;
      if (WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        memcpy(bestBssid, WiFi.BSSID(i), 6);
        bestChannel = WiFi.channel(i);
        haveBest = true;
      }
    }
    Serial.printf("  %s '%s'  %d dBm  csatorna: %d  %s  BSSID: %s\n",
                  match ? ">>" : "  ",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  encText(WiFi.encryptionType(i)), WiFi.BSSIDstr(i).c_str());
  }
  if (n == 0) Serial.println("  Egy halozatot sem lat!");
  Serial.printf("Keresett halozat: '%s' (%d karakter), jelszo hossza: %d karakter\n",
                WIFI_SSID, strlen(WIFI_SSID), strlen(WIFI_PASS));
  Serial.println(found ? "A keresett halozat LATHATO (>> jeloli)."
                       : "A keresett halozat NINCS a listaban!");
  WiFi.scanDelete();
}

void connectWiFi() {
  WiFi.persistent(false);       // ne a flashben tárolt régi adatokat használja
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  if (FORCE_11G) {
    WiFi.setPhyMode(WIFI_PHY_MODE_11G);
    Serial.println("PHY mod: 802.11g");
  }

  disconnectedHandler = WiFi.onStationModeDisconnected(
    [](const WiFiEventStationModeDisconnected& e) {
      associated = false;
      Serial.printf("  leszakadt, ok: %d - %s\n", e.reason, reasonText(e.reason));
    });

  connectedHandler = WiFi.onStationModeConnected(
    [](const WiFiEventStationModeConnected& e) {
      associated = true;
      Serial.printf("  TARSITVA a jeladoval (csatorna %d) -> a jelszo JO, IP-cimre var...\n", e.channel);
    });

  gotIpHandler = WiFi.onStationModeGotIP(
    [](const WiFiEventStationModeGotIP& e) {
      Serial.printf("  IP-cim megerkezett: %s\n", e.ip.toString().c_str());
    });

#ifdef STATIC_IP
  // Fix IP-cím: nem kér címet a routertől (DHCP nélkül)
  IPAddress ip, gw, sn, dns;
  ip.fromString(STATIC_IP);
  gw.fromString(STATIC_GATEWAY);
  sn.fromString(STATIC_SUBNET);
  dns.fromString(STATIC_GATEWAY);
  WiFi.config(ip, gw, sn, dns);
  Serial.printf("Fix IP-cim: %s, atjaro: %s\n", STATIC_IP, STATIC_GATEWAY);
#else
  Serial.println("IP-cim: DHCP (a routertol ker)");
#endif

  scanNetworks();

  while (true) {
    associated = false;
    if (haveBest) {
      // Több azonos nevű jeladó (mesh/repeater) esetén a legerősebbhez kapcsolódunk
      Serial.printf("Csatlakozas: '%s' (legerosebb jelado, csatorna %d)\n", WIFI_SSID, bestChannel);
      WiFi.begin(WIFI_SSID, WIFI_PASS, bestChannel, bestBssid);
    } else {
      Serial.printf("Csatlakozas: '%s'\n", WIFI_SSID);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }

    uint32_t start = millis();
    wl_status_t last = (wl_status_t)255;
    // Ha már társítva van, több időt adunk az IP-címre
    while (millis() - start < (associated ? 2 * WIFI_TIMEOUT_MS : WIFI_TIMEOUT_MS)) {
      wl_status_t s = WiFi.status();
      if (s == WL_CONNECTED) {
        Serial.print("Kapcsolodva, IP-cim: ");
        Serial.println(WiFi.localIP());
        return;
      }
      if (s != last) {
        Serial.printf("  allapot: %d - %s\n", s, statusText(s));
        last = s;
      }
      delay(250);
    }
    Serial.println("Idotullepes, ujraprobalom...\n");
    WiFi.disconnect();
    delay(500);
  }
}

// Háromszög-jel az idő függvényében: 1 mp oda, 1 mp vissza (nem blokkol)
void updateServo() {
  if (!servoEnabled) return;
  uint32_t phase = millis() % (2 * SWEEP_TIME_MS);
  uint32_t t = (phase < SWEEP_TIME_MS) ? phase : (2 * SWEEP_TIME_MS - phase);
  int angle = SWEEP_MIN + (int)((uint32_t)(SWEEP_MAX - SWEEP_MIN) * t / SWEEP_TIME_MS);
  if (angle != lastServoAngle) {
    servo.write(angle);
    lastServoAngle = angle;
  }
}

// ---------- Ultrahang ----------
int measureCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return -1;
  return (int)(us * 0.0343f / 2.0f);
}

// ---------- UDP ----------
void sendToPC(const char* msg) {
  if (!havePC) return;
  udp.beginPacket(pcIP, pcPort);
  udp.write((const uint8_t*)msg, strlen(msg));
  udp.endPacket();
}

void handlePackets() {
  while (udp.parsePacket() > 0) {
    char buf[32];
    int n = udp.read(buf, sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = 0;
    if (strncmp(buf, "HELLO", 5) == 0) {
      if (!havePC || pcIP != udp.remoteIP()) {
        Serial.printf("PC csatlakozott: %s\n", udp.remoteIP().toString().c_str());
      }
      pcIP = udp.remoteIP();
      pcPort = udp.remotePort();
      havePC = true;
      sendToPC("HELLO OK");
    }
  }
}

// Mérés az aktuális servo szögnél, és küldés a PC-nek
void updateSensor() {
  if (millis() - lastMeasureMs < MEASURE_INTERVAL_MS) return;
  lastMeasureMs = millis();

  int angle = lastServoAngle;
  lastDistance = measureCm();

  char msg[24];
  snprintf(msg, sizeof(msg), "R %d %d", angle, lastDistance);
  sendToPC(msg);

  // Soros monitorra csak félmásodpercenként, hogy olvasható maradjon
  if (millis() - lastDebugMs > 500) {
    lastDebugMs = millis();
    Serial.printf("Szog: %3d  Tavolsag: %d cm\n", angle, lastDistance);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    // Feltöltés közben a loop nem fut: a servót elengedjük, hogy ne rángasson
    servoEnabled = false;
    servo.detach();
    Serial.println("OTA: feltoltes indul");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: kesz, ujraindulas");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", progress * 100 / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA hiba: %u\n", error);
    servo.attach(PIN_SERVO, SERVO_MIN_US, SERVO_MAX_US);
    servoEnabled = true;
  });

  ArduinoOTA.begin();
  Serial.print("OTA kesz, hostname: ");
  Serial.println(OTA_HOSTNAME);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Lanctalpas robot indul ===");

  servo.attach(PIN_SERVO, SERVO_MIN_US, SERVO_MAX_US);
  servo.write((SWEEP_MIN + SWEEP_MAX) / 2);   // induláskor középre áll

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  connectWiFi();
  setupOTA();

  udp.begin(UDP_PORT);
  Serial.printf("UDP port: %d\n", UDP_PORT);
  Serial.println("Servo pasztazas es meres indul");
}

void loop() {
  ArduinoOTA.handle();
  updateServo();
  handlePackets();
  updateSensor();

  // 5 másodpercenként kiírja az állapotot
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi OK, IP: %s, jelerosseg: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      Serial.printf("WiFi: %s\n", statusText(WiFi.status()));
    }
  }
}