#pragma once
#include <Arduino.h>
#include "gfx_config.h"
#include "tx_history.h"

enum class Screen {
    SPLASH, SETUP_INFO, AMOUNT_ENTRY, LOADING, QR_DISPLAY, PAID, ERROR,
    SCREENSAVER, STORE_SELECT, TXN_HISTORY,
};

enum class Key {
    NONE,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    DOT, DEL, CLEAR, CHARGE,
    TEST_PRINT,
    MENU,           // top-right menu button on the amount screen
    CANCEL,         // cancel button on the invoice (QR) screen
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
    void showPaid(uint64_t sats, float nzd, const String& currency = "NZD");
    void showError(const String& message);
    void showWifiError(const String& ssid);

    /// Transaction history: timeframe tabs + a scrollable list of individual
    /// transactions for the selected period, with a count + paid total.
    /// Call resetHistoryView() before the first show() to reset tab/scroll.
    void resetHistoryView();
    void showTransactionHistory(const HistoryData& d, const String& currency);

    // Result of a touch on the history screen.
    enum class HistEvent { NONE, BACK, CHECK, UPDATE };
    /// Handle a touch (tabs / scroll / back / a row's Check button). Re-renders
    /// internally on tab/scroll. On CHECK, outRecordIdx is the d.all index of
    /// the tapped transaction and the button shows a "..." spinner.
    HistEvent pollTransactionHistory(const HistoryData& d, const String& currency,
                                     int& outRecordIdx);
    /// Paint the live-check outcome on the row whose Check button was tapped.
    void showCheckResult(InvoiceState state);

    /// Show the last NFC tap (UID + NDEF URL) in a diagnostic band on the
    /// setup screen, so bench NFC testing needs no serial monitor.
    void showNfcTap(const String& uid, const String& url, int count);

    /// Paint the splash screensaver bitmap. Tap exits.
    void showScreensaver();

    void updateTimer(int secsLeft, int refreshCount);

    /// BTCPay store picker (shown after reboot when the store isn't chosen
    /// yet). Draws up to a screenful of tappable store rows.
    void showStoreSelect(const String* names, int count);
    /// Returns the tapped store row index, or -1 if no tap this poll.
    int  pollStoreSelect(int count);
    /// Number of store rows that fit on screen (caller should cap to this).
    static int storeSelectCapacity();

    Key  pollTouch();
    bool anyTouch();

    /// Debounced raw touch read for ad-hoc screens drawn outside DisplayUI
    /// (e.g. the firmware-update menu). Returns true once per press with
    /// the touch coordinates.
    bool touchPoint(uint16_t& x, uint16_t& y);

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

    // Transaction-history view state (selected tab + scroll page).
    Timeframe _histTf    = Timeframe::DAY;
    int       _histPage  = 0;
    int       _checkBtnY = -1;   // y of the Check button last tapped

    void drawHeader(const String& text);
    void drawMenuButton();
    void drawCheckButton(int y, const String& label, uint16_t bg, uint16_t fg);
    void drawButton(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg, int textSize);
    void drawCenteredText(const String& s, int cx, int cy, int size, uint16_t fg, uint16_t bg);
    Key  hitTest(int tx, int ty);
    void drawQRCode(const String& data, int x, int y, int areaSize);
};
