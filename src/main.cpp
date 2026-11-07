// Lánctalpas robot – ESP8266
// 1. lépés: csak WiFi és OTA
//
// Első feltöltés USB-n (env: robot), utána WiFi-n (env: robot_ota).

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Hianyzik az include/secrets.h! Masoljatok le az include/secrets.h.example fajlt secrets.h neven, es toltsetek ki."
#endif

uint32_t lastStatusMs = 0;

void connectWiFi() {
  Serial.print("Csatlakozas: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Kapcsolodva, IP-cim: ");
  Serial.println(WiFi.localIP());
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
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
  });

  ArduinoOTA.begin();
  Serial.print("OTA kesz, hostname: ");
  Serial.println(OTA_HOSTNAME);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  connectWiFi();
  setupOTA();
}

void loop() {
  ArduinoOTA.handle();

  // 5 másodpercenként kiírja az állapotot (ha később nyitjátok meg a soros monitort)
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi OK, IP: %s, jelerosseg: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      Serial.println("WiFi: nincs kapcsolat, ujracsatlakozas...");
    }
  }
}