// ============================================================
// ESP32-C6 co-processor OTA updater (ESP32-P4-Function-EV-Board).
//
// Streams the embedded esp_hosted 2.12.3 C6 image (in LittleFS) to the C6
// over the existing SDIO link and activates it. No WiFi association needed —
// only the SDIO transport, which comes up regardless of the (currently buggy)
// WiFi. One-shot: flash, run once, watch for "DONE", then reflash the POS.
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

extern "C" {
#include "esp_hosted_ota.h"
}

#define FW_PATH "/esp32c6-v2.12.3.bin"

void setup() {
    Serial.begin(115200);
    delay(1200);
    Serial.println("\n========================================");
    Serial.println("  ESP32-C6 co-processor OTA updater");
    Serial.println("========================================");

    // Bring up the ESP-Hosted SDIO transport to the C6. WiFi assoc may fail
    // (that's the bug we're fixing); the transport still comes up, which is all
    // the OTA needs. Watch for the version log + "ESP-Hosted initialized!".
    WiFi.mode(WIFI_STA);
    delay(3000);

    if (!LittleFS.begin(false)) {
        Serial.println("[OTA] LittleFS mount failed.");
        Serial.println("[OTA] Did you flash the filesystem?  pio run -t uploadfs");
        return;
    }

    File f = LittleFS.open(FW_PATH, "r");
    if (!f) {
        Serial.println("[OTA] " FW_PATH " not found in LittleFS.");
        return;
    }
    size_t total = f.size();
    Serial.printf("[OTA] image: %u bytes\n", (unsigned)total);

    Serial.println("[OTA] begin (this streams ~1.2MB over SDIO; takes a bit)...");
    if (esp_hosted_slave_ota_begin() != ESP_OK) {
        Serial.println("[OTA] esp_hosted_slave_ota_begin() FAILED");
        f.close();
        return;
    }

    uint8_t buf[4096];
    size_t  sent = 0;
    int     lastPct = -1;
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        if (esp_hosted_slave_ota_write(buf, (uint32_t)n) != ESP_OK) {
            Serial.printf("[OTA] write FAILED at %u bytes\n", (unsigned)sent);
            f.close();
            return;
        }
        sent += n;
        int pct = (int)(100ULL * sent / total);
        if (pct != lastPct && pct % 10 == 0) {
            Serial.printf("[OTA]   %d%%  (%u/%u)\n", pct, (unsigned)sent, (unsigned)total);
            lastPct = pct;
        }
    }
    f.close();

    Serial.println("[OTA] end...");
    if (esp_hosted_slave_ota_end() != ESP_OK) {
        Serial.println("[OTA] esp_hosted_slave_ota_end() FAILED");
        return;
    }

    Serial.println("[OTA] activating (the C6 will reboot into the new firmware)...");
    esp_hosted_slave_ota_activate();

    Serial.println("[OTA] ==== DONE ====");
    Serial.println("[OTA] Power-cycle the board, then reflash the POS firmware.");
}

void loop() {}
