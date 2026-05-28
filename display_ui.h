#pragma once
#include <Arduino.h>
#include "gfx_config.h"

enum class Screen {
    SPLASH, SETUP_INFO, AMOUNT_ENTRY, LOADING, QR_DISPLAY, PAID, ERROR,
    SCREENSAVER,
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
    void showWifiError(const String& ssid);

    /// Paint the splash screensaver bitmap. Tap exits.
    void showScreensaver();

    void updateTimer(int secsLeft, int refreshCount);

    /// Sample the battery and repaint the corner icon if the percentage
    /// changed. Cheap to call every loop iteration.
    void tickBattery();

    /// Force-redraw the battery icon now (e.g. after a screen transition
    /// painted over its corner).
    void drawBatteryIcon();

    Key  pollTouch();
    bool anyTouch();

    Screen    screen() const { return _screen; }
    Arduino_GFX* gfx() { return _gfx; }

    /// Set the text font + scale based on the legacy 1..10 size.
    /// Use this in place of gfx()->setTextSize() so the proportional sans
    /// fonts are used everywhere instead of the default 5x7 bitmap.
    void applyTextSize(int size);

    /// Set the GFX cursor so the next print() draws with its visual
    /// top-left at (x, yTop). Required because GFXfonts use baseline
    /// positioning, not top-left like the default 5x7 font.
    void setCursorTopLeft(int x, int yTop, int size);

private:
    Arduino_GFX* _gfx = nullptr;
    Screen _screen = Screen::SPLASH;
    bool   _wasTouched = false;
    unsigned long _lastTouch = 0;

    // Background colour of whatever the battery icon overlays on the
    // current screen — set by each show*() call so tickBattery() can
    // repaint cleanly without flickering.
    uint16_t _iconBg = 0x0000;
    int      _iconLastPercent = -2;     // -2 = nothing drawn yet
    bool     _iconLastCharging = false;
    bool     _iconHidden = true;

    void drawHeader(const String& text);
    void drawButton(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg, int textSize);
    void drawCenteredText(const String& s, int cx, int cy, int size, uint16_t fg, uint16_t bg);
    Key  hitTest(int tx, int ty);
    void drawQRCode(const String& data, int x, int y, int areaSize);
};
