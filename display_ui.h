#pragma once
#include <Arduino.h>
#include "gfx_config.h"

enum class Screen {
    SPLASH, SETUP_INFO, AMOUNT_ENTRY, LOADING, QR_DISPLAY, PAID, ERROR,
};

enum class Key {
    NONE,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    DOT, DEL, CLEAR, CHARGE,
    TEST_PRINT,
};

class DisplayUI {
public:
    void begin();

    /// Run 2-point calibration. Blocks; times out after 10s per target.
    void calibrateTouch();

    void showSplash(const String& merchantName = "");
    void showSetupInfo();
    void showAmountEntry(const String& amount, const String& currency);
    void showLoading(const String& message);
    void showQR(const String& bolt11, uint64_t sats, float nzd, int secsLeft, int refreshCount);
    void showPaid(uint64_t sats, float nzd);
    void showError(const String& message);

    void updateTimer(int secsLeft, int refreshCount);

    Key  pollTouch();
    bool anyTouch();

    Screen    screen() const { return _screen; }
    Arduino_GFX* gfx() { return _gfx; }

private:
    Arduino_GFX* _gfx = nullptr;
    Screen _screen = Screen::SPLASH;
    bool   _wasTouched = false;
    unsigned long _lastTouch = 0;

    void drawHeader(const String& text);
    void drawButton(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg, int textSize);
    void drawCenteredText(const String& s, int cx, int cy, int size, uint16_t fg, uint16_t bg);
    Key  hitTest(int tx, int ty);
    void drawQRCode(const String& data, int x, int y, int areaSize);
};
