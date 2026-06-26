#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ============================================================
// DisplayUI (ESP32-S3, 240x320 ILI9341 + FT6336 capacitive touch).
// Screen rendering + touch hit-testing for the POS. Mirrors the P4
// DisplayUI's role but laid out for the smaller portrait panel.
// ============================================================

enum class Key {
    NONE,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    DOT, DEL, CLEAR, CHARGE,
};

class DisplayUI {
public:
    void begin();

    void showSplash(const String& merchantName = "");
    void showSetupInfo(const String& ssid, const String& ip);
    void showAmountEntry(const String& amount, const String& currency);
    void showMessage(const String& title, const String& body, uint16_t accent);
    void showLoading(const String& message);
    void showQR(const String& bolt11, uint64_t sats, float fiat,
                const String& currency, int secsLeft);
    void updateCountdown(int secsLeft);
    void showPaid(uint64_t sats, float fiat, const String& currency);
    void showError(const String& message);

    /// Returns the tapped key on the amount-entry numpad (NONE if none).
    Key  pollTouch();
    /// True on a fresh tap anywhere (for dismiss screens).
    bool anyTouch();

    Arduino_GFX* gfx() { return _gfx; }

private:
    Arduino_GFX* _gfx = nullptr;
    bool         _touchDown = false;
    uint32_t     _lastTouchMs = 0;

    bool readTouchPoint(int& x, int& y);
    Key  hitTest(int x, int y);

    void drawButton(int x, int y, int w, int h, const String& label,
                    uint16_t bg, uint16_t fg, int textSize);
    void centerText(const String& s, int cx, int cy, int size, uint16_t fg);
    void drawQRCode(const String& data, int x, int y, int areaSize);
};
