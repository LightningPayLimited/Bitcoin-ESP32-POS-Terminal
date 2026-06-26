#include "display_ui.h"
#include "config.h"
#include "setup_portal.h"
#include "logo.h"
#include "splash_image.h"
#include "pos_fonts.h"
#include <qrcode.h>

// ============================================================
// Layout constants — 800x480 landscape (device mounted on its right side)
// ============================================================
#define HDR_H      46

// Amount entry: amount panel on the left, keypad on the right.
#define AMT_BOX_X  12
#define AMT_BOX_W  376
#define AMT_BOX_Y  175
#define AMT_BOX_H  120
#define KP_X0      400
#define KP_Y0      54
#define KP_COLS    4
#define KP_ROWS    4
#define KP_BW      93
#define KP_BH      100
#define KP_GAP     6

// Test Print button on the SETUP_INFO screen
#define TP_W       300
#define TP_H       60
#define TP_X       ((SCREEN_WIDTH - TP_W) / 2)
#define TP_Y       400

// Menu button in the top-right of the amount-entry header (opens history)
#define MENU_W     54
#define MENU_H     38
#define MENU_X     (SCREEN_WIDTH - MENU_W - 8)
#define MENU_Y     ((HDR_H - MENU_H) / 2)

// Cancel button — lives in the right-hand details column of the QR screen.
// The only way to cancel a sale, so a mistap elsewhere can't close it.
#define CANCEL_W   300
#define CANCEL_X   478
#define CANCEL_Y   404
#define CANCEL_H   58

// Extra touch slop for small buttons near the screen edges (beyond the touch
// calibration range). The hit area extends past the drawn button.
#define TPAD_UP    14
#define TPAD_DN    24

// --- Transaction history screen ---
// Back button (top-left of the header)
#define BACK_W     78
#define BACK_H     38
#define BACK_X     6
#define BACK_Y     ((HDR_H - BACK_H) / 2)

// Timeframe tabs (4 across, just under the header)
#define TAB_Y      52
#define TAB_H      42
#define TAB_MARGIN 8
#define TAB_GAP    6
#define TAB_W      ((SCREEN_WIDTH - 2 * TAB_MARGIN - 3 * TAB_GAP) / 4)
#define TAB_COUNT  4
#define TAB_X(i)   (TAB_MARGIN + (i) * (TAB_W + TAB_GAP))

// Transaction list area — single-line rows across the wide screen.
#define TXL_TOP    124
#define TXL_BOTTOM 428
#define TXL_ROW_H  50
#define TXL_XL     16
#define TXL_XR     (SCREEN_WIDTH - 16)

// Per-row "Check" button (re-checks live invoice status)
#define CHK_W      92
#define CHK_H      40
#define CHK_X      (SCREEN_WIDTH - 16 - CHK_W)
#define CHK_YOFF   ((TXL_ROW_H - CHK_H) / 2)   // y within a row
// Left content stops just before the Check button.
#define TXL_XR2    (CHK_X - 12)

// Scroll footer (prev / page indicator / next)
#define SCR_Y      434
#define SCR_H      42
#define SCR_PREV_X 8
#define SCR_NEXT_X (SCREEN_WIDTH - 128)
#define SCR_BTN_W  120

static const char* KP[KP_ROWS][KP_COLS] = {
    {"1", "2", "3", "<"},
    {"4", "5", "6", "C"},
    {"7", "8", "9", "."},
    {"",  "0", "",  "$"},
};

static Key keyMap(int r, int c) {
    if (r==0) { if(c==0) return Key::D1; if(c==1) return Key::D2; if(c==2) return Key::D3; if(c==3) return Key::DEL; }
    if (r==1) { if(c==0) return Key::D4; if(c==1) return Key::D5; if(c==2) return Key::D6; if(c==3) return Key::CLEAR; }
    if (r==2) { if(c==0) return Key::D7; if(c==1) return Key::D8; if(c==2) return Key::D9; if(c==3) return Key::DOT; }
    if (r==3) { if(c==1) return Key::D0; if(c==3) return Key::CHARGE; }
    return Key::NONE;
}

// ============================================================
// begin()
// ============================================================
static bool waitRawTap(int32_t* outRawX, int32_t* outRawY, uint32_t timeoutMs) {
    uint16_t tx, ty;
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (gt911ReadTouch(&tx, &ty)) {
            *outRawX = gt911LastRawX();
            *outRawY = gt911LastRawY();
            // Wait for release (up to 2s)
            uint32_t rstart = millis();
            while (gt911ReadTouch(&tx, &ty) && millis() - rstart < 2000) {
                delay(15);
            }
            return true;
        }
        delay(10);
    }
    return false;
}

void DisplayUI::calibrateTouch() {
    const int inset = 60;
    const int targets[2][2] = {
        { inset,             inset               },
        { SCREEN_WIDTH-inset, SCREEN_HEIGHT-inset }
    };
    const char* labels[2] = { "TAP TOP-LEFT CROSS", "TAP BOTTOM-RIGHT CROSS" };
    int32_t raw[2][2];

    for (int step = 0; step < 2; step++) {
        while (true) {
            _gfx->fillScreen(COL_BG);
            drawCenteredText("TOUCH CALIBRATION", SCREEN_WIDTH/2, 120,
                             3, COL_ACCENT, COL_BG);
            drawCenteredText(labels[step], SCREEN_WIDTH/2, SCREEN_HEIGHT/2 - 40,
                             2, COL_FG, COL_BG);
            drawCenteredText("(tap the orange target)",
                             SCREEN_WIDTH/2, SCREEN_HEIGHT/2 + 20,
                             1, COL_DIM, COL_BG);

            int cx = targets[step][0], cy = targets[step][1];
            _gfx->drawLine(cx - 40, cy, cx + 40, cy, COL_ACCENT);
            _gfx->drawLine(cx, cy - 40, cx, cy + 40, COL_ACCENT);
            _gfx->drawCircle(cx, cy, 30, COL_ACCENT);
            _gfx->drawCircle(cx, cy, 15, COL_ACCENT);
            _gfx->fillCircle(cx, cy, 5, COL_ACCENT);

            if (waitRawTap(&raw[step][0], &raw[step][1], 30000)) {
                Serial.printf("[CAL] target %d (scr=%d,%d): raw=%ld,%ld\n",
                              step, targets[step][0], targets[step][1],
                              (long)raw[step][0], (long)raw[step][1]);
                _gfx->fillCircle(cx, cy, 22, 0x07E0);
                delay(400);
                break;
            } else {
                _gfx->fillScreen(COL_BG);
                drawCenteredText("NO TAP DETECTED", SCREEN_WIDTH/2,
                                 SCREEN_HEIGHT/2, 3, COL_ERROR, COL_BG);
                drawCenteredText("retrying...", SCREEN_WIDTH/2,
                                 SCREEN_HEIGHT/2 + 50, 2, COL_DIM, COL_BG);
                delay(1500);
                // loop back and try again
            }
        }
    }

    // Map the two corner taps to the landscape calibration endpoints. The TL
    // tap is screen (0,0), the BR tap is (W-1,H-1). With TOUCH_SWAP_XY the raw
    // Y drives screen X (and raw X drives screen Y).
    auto& c = touchCalib();
#if TOUCH_SWAP_XY
    c.xRaw0 = raw[0][1]; c.xRaw1 = raw[1][1];   // screen X <- raw Y
    c.yRaw0 = raw[0][0]; c.yRaw1 = raw[1][0];   // screen Y <- raw X
#else
    c.xRaw0 = raw[0][0]; c.xRaw1 = raw[1][0];
    c.yRaw0 = raw[0][1]; c.yRaw1 = raw[1][1];
#endif
    c.done = true;
    Serial.printf("[CAL] done xRaw[%ld..%ld] yRaw[%ld..%ld]\n",
                  (long)c.xRaw0, (long)c.xRaw1, (long)c.yRaw0, (long)c.yRaw1);
}

void DisplayUI::begin() {
    panelPowerOn();
    _gfx = createDSIDisplay();
    _gfx->begin();
    _gfx->fillScreen(COL_BG);
    _gfx->flush(true);
    touchBegin();
}

// ============================================================
// Helpers
// ============================================================
void DisplayUI::applyTextSize(int size) {
    applyPosFont(_gfx, size);
}

void DisplayUI::setCursorTopLeft(int x, int yTop, int size) {
    applyPosFont(_gfx, size);
    int16_t x1, y1;
    uint16_t w, h;
    // Sample text with both ascender and descender so we land on a
    // sensible baseline regardless of which characters get drawn next.
    _gfx->getTextBounds("Mg", 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(x, yTop - y1);  // y1 is negative for GFXfonts
}

void DisplayUI::drawCenteredText(const String& s, int cx, int cy, int size,
                                 uint16_t fg, uint16_t bg) {
    applyPosFont(_gfx, size);
    _gfx->setTextColor(fg, bg);
    int16_t x1, y1;
    uint16_t w, h;
    _gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
    // GFXfonts return a negative y1 because the bounding box extends above
    // the cursor's baseline. Subtract the offsets so the rendered glyphs are
    // actually centred on (cx, cy).
    _gfx->setCursor(cx - w / 2 - x1, cy - h / 2 - y1);
    _gfx->print(s);
}

void DisplayUI::drawHeader(const String& text) {
    _gfx->fillRect(0, 0, SCREEN_WIDTH, HDR_H, COL_HEADER_BG);
    drawCenteredText(text, SCREEN_WIDTH / 2, HDR_H / 2, 3, COL_BG, COL_HEADER_BG);
}

// Hamburger menu button drawn over the right end of the orange header.
void DisplayUI::drawMenuButton() {
    _gfx->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 7, COL_BG);
    int barW = MENU_W - 22;
    int x0   = MENU_X + 11;
    for (int i = 0; i < 3; i++) {
        int y = MENU_Y + 11 + i * 8;
        _gfx->fillRect(x0, y, barW, 3, COL_ACCENT);
    }
}

void DisplayUI::drawButton(int x, int y, int w, int h,
                           const char* label, uint16_t bg, uint16_t fg, int textSize) {
    _gfx->fillRoundRect(x + 2, y + 2, w - 4, h - 4, 8, bg);
    const char* draw = label;
    if      (strcmp(label, "$") == 0) draw = "PAY";
    else if (strcmp(label, "<") == 0) draw = "DEL";
    else if (strcmp(label, "C") == 0) draw = "CLR";
    drawCenteredText(draw, x + w / 2, y + h / 2, textSize, fg, bg);
}

// ============================================================
// Screens
// ============================================================
void DisplayUI::showSplash(const String& merchantName) {
    _screen = Screen::SPLASH;
    // Full-screen 800x480 branded splash bitmap. (The design is fixed, so the
    // optional merchant name isn't overlaid — it would clobber the artwork.)
    (void)merchantName;
    _gfx->draw16bitRGBBitmap(0, 0, SPLASH_RGB565, SPLASH_W, SPLASH_H);
}

void DisplayUI::showSetupInfo() {
    _screen = Screen::SETUP_INFO;
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ACCENT);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ACCENT);

    // Lightning Pay wordmark at the top.
    _gfx->draw16bitRGBBitmap((SCREEN_WIDTH - LOGO_W) / 2, 14, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("POS SETUP", SCREEN_WIDTH / 2, 92, 4, COL_ACCENT, COL_BG);

    drawCenteredText("Connect to WiFi:", SCREEN_WIDTH / 2, 138, 2, COL_FG, COL_BG);
    drawCenteredText(SetupPortal::apSSID(), SCREEN_WIDTH / 2, 170, 3, COL_ACCENT, COL_BG);

    drawCenteredText("Then open browser to:", SCREEN_WIDTH / 2, 212, 2, COL_FG, COL_BG);
    drawCenteredText(SETUP_AP_IP,             SCREEN_WIDTH / 2, 244, 3, COL_ACCENT, COL_BG);

    // NFC diagnostic band (overwritten by showNfcTap on each card tap).
    drawCenteredText("Tap NFC card to test", SCREEN_WIDTH / 2, 318, 2, COL_DIM, COL_BG);

    // Test Print button — exercise the thermal printer without a real sale
    _gfx->fillRoundRect(TP_X, TP_Y, TP_W, TP_H, 10, COL_KEYPAD_BG);
    _gfx->drawRoundRect(TP_X, TP_Y, TP_W, TP_H, 10, COL_ACCENT);
    drawCenteredText("Test Print", SCREEN_WIDTH / 2, TP_Y + TP_H / 2,
                     3, COL_ACCENT, COL_KEYPAD_BG);
}

// Diagnostic readout for NFC bench testing: fills the band below the setup
// screen's browser-IP line with the last tap's UID (and a truncated NDEF URL),
// so a reader can be tested without a serial monitor.
void DisplayUI::showNfcTap(const String& uid, const String& url, int count) {
    const int top = 288, h = 90;
    _gfx->fillRect(0, top, SCREEN_WIDTH, h, COL_BG);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "NFC tap #%d", count);
    drawCenteredText(hdr, SCREEN_WIDTH / 2, top + 14, 2, COL_DIM, COL_BG);
    drawCenteredText(uid.length() ? uid : "----",
                     SCREEN_WIDTH / 2, top + 50, 3, COL_ACCENT, COL_BG);

    if (url.length()) {
        String u = url.length() > 34 ? url.substring(0, 33) + "..." : url;
        drawCenteredText(u, SCREEN_WIDTH / 2, top + 84, 1, COL_FG, COL_BG);
    } else {
        drawCenteredText("(no NDEF URL)", SCREEN_WIDTH / 2, top + 84, 1, COL_DIM, COL_BG);
    }
}

void DisplayUI::showAmountEntry(const String& amount, const String& currency) {
    // When we're already on this screen and just changing the amount, skip
    // the full repaint — the header + keypad don't change, and the screen-
    // wide fillScreen() is what causes the visible black flash on every key.
    const bool alreadyHere = (_screen == Screen::AMOUNT_ENTRY);
    _screen = Screen::AMOUNT_ENTRY;
    if (!alreadyHere) {
        _gfx->fillScreen(COL_BG);
        drawHeader("Lightning Pay");
        drawMenuButton();
        // Left-panel label above the amount box (static).
        drawCenteredText("Enter amount", AMT_BOX_X + AMT_BOX_W / 2, 130, 2,
                         COL_DIM, COL_BG);
    }

    // Amount box on the left panel (redraw wipes the previous amount text).
    _gfx->fillRoundRect(AMT_BOX_X, AMT_BOX_Y, AMT_BOX_W, AMT_BOX_H, 8, COL_KEYPAD_BG);
    _gfx->drawRoundRect(AMT_BOX_X, AMT_BOX_Y, AMT_BOX_W, AMT_BOX_H, 8, COL_ACCENT);

    String disp = amount.isEmpty() ? "0.00" : amount;
    String right = disp + " " + currency;

    // GFXfont baseline-positioning: use getTextBounds + (x1,y1) so "$" and
    // the amount string sit visually centred inside the rounded box.
    int boxCy = AMT_BOX_Y + AMT_BOX_H / 2;
    int16_t x1, y1; uint16_t w, h;

    applyPosFont(_gfx, 4);
    _gfx->setTextColor(COL_ACCENT, COL_KEYPAD_BG);
    _gfx->getTextBounds("$", 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(AMT_BOX_X + 16 - x1, boxCy - h / 2 - y1);
    _gfx->print("$");

    _gfx->setTextColor(COL_FG, COL_KEYPAD_BG);
    _gfx->getTextBounds(right.c_str(), 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(AMT_BOX_X + AMT_BOX_W - 16 - w - x1, boxCy - h / 2 - y1);
    _gfx->print(right);

    // Keypad — static between keystrokes, only paint on first entry.
    if (!alreadyHere) {
        for (int r = 0; r < KP_ROWS; r++) {
            for (int c = 0; c < KP_COLS; c++) {
                const char* lbl = KP[r][c];
                if (strlen(lbl) == 0) continue;

                int x = KP_X0 + c * (KP_BW + KP_GAP);
                int y = KP_Y0 + r * (KP_BH + KP_GAP);
                uint16_t bg = COL_KEYPAD_BG, fg = COL_KEYPAD_FG;

                if (strcmp(lbl, "$") == 0) { bg = COL_ACCENT; fg = COL_BG; }
                else if (strcmp(lbl, "<") == 0 || strcmp(lbl, "C") == 0) { bg = 0x3186; }

                drawButton(x, y, KP_BW, KP_BH, lbl, bg, fg, 3);
            }
        }
    }
}

void DisplayUI::showLoading(const String& message) {
    _screen = Screen::LOADING;
    _gfx->fillScreen(COL_BG);
    drawHeader("Lightning Pay");
    drawCenteredText(message, SCREEN_WIDTH / 2, 210, 3, COL_FG, COL_BG);
    drawCenteredText(". . .", SCREEN_WIDTH / 2, 280, 3, COL_ACCENT, COL_BG);
}

// Right-hand details column on the QR screen.
#define QR_RC_CX   636

void DisplayUI::showQR(const String& bolt11, uint64_t sats, float nzd,
                       int secsLeft, int refreshCount) {
    _screen = Screen::QR_DISPLAY;
    _gfx->fillScreen(COL_BG);

    // QR on the left.
    int qrSize = 440;
    int qrX = 16;
    int qrY = 20;
    String qrData = bolt11;
    qrData.toUpperCase();
    drawQRCode(qrData, qrX, qrY, qrSize);

    // Details column on the right.
    if (nzd > 0) {
        char s[16]; snprintf(s, sizeof(s), "$%.2f NZD", nzd);
        drawCenteredText(s, QR_RC_CX, 52, 4, COL_ACCENT, COL_BG);
    }
    char satStr[32]; snprintf(satStr, sizeof(satStr), "%lu sats", (unsigned long)sats);
    drawCenteredText(satStr, QR_RC_CX, 118, 2, COL_FG, COL_BG);

    _gfx->drawFastHLine(478, 150, SCREEN_WIDTH - 478 - 14, COL_DIM);

    drawCenteredText("Scan with any",     QR_RC_CX, 184, 2, COL_FG, COL_BG);
    drawCenteredText("Lightning wallet",  QR_RC_CX, 214, 2, COL_FG, COL_BG);

    updateTimer(secsLeft, refreshCount);

    // Cancel button (only this cancels — mistaps elsewhere are ignored).
    _gfx->fillRoundRect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 10, COL_KEYPAD_BG);
    _gfx->drawRoundRect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 10, COL_ERROR);
    drawCenteredText("Cancel", CANCEL_X + CANCEL_W / 2, CANCEL_Y + CANCEL_H / 2, 3,
                     COL_ERROR, COL_KEYPAD_BG);
}

void DisplayUI::updateTimer(int secsLeft, int refreshCount) {
    // Clear only the right-column timer band (x 472+, y 268..390) so the QR,
    // the details above, and the Cancel button below are never touched.
    _gfx->fillRect(472, 268, SCREEN_WIDTH - 472, 122, COL_BG);

    char t[8]; snprintf(t, sizeof(t), ":%02d", secsLeft);
    drawCenteredText(t, QR_RC_CX, 305, 7,
                     secsLeft < 10 ? COL_ERROR : COL_FG, COL_BG);

    if (refreshCount > 0) {
        char r[32];
        snprintf(r, sizeof(r), "refresh %d/%d", refreshCount, MAX_INVOICE_REFRESHES);
        drawCenteredText(r, QR_RC_CX, 360, 2, COL_DIM, COL_BG);
    }
}

void DisplayUI::showPaid(uint64_t sats, float nzd, const String& currency) {
    _screen = Screen::PAID;
    _gfx->fillScreen(COL_BG);

    int cx = SCREEN_WIDTH / 2;

    int logoX = (SCREEN_WIDTH - LOGO_W) / 2;
    int logoY = 64;
    _gfx->draw16bitRGBBitmap(logoX, logoY, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("PAID", cx, 210, 7, COL_ACCENT, COL_BG);

    char info[64];
    if (nzd > 0) snprintf(info, sizeof(info), "$%.2f %s", nzd, currency.c_str());
    else         snprintf(info, sizeof(info), "%lu sats", (unsigned long)sats);
    drawCenteredText(info, cx, 300, 3, COL_FG, COL_BG);
    if (nzd > 0) {
        char s[32]; snprintf(s, sizeof(s), "%lu sats", (unsigned long)sats);
        drawCenteredText(s, cx, 345, 2, COL_DIM, COL_BG);
    }

    drawCenteredText("Tap to continue", cx, SCREEN_HEIGHT - 28, 2, COL_DIM, COL_BG);
}

void DisplayUI::showWifiError(const String& ssid) {
    _screen = Screen::ERROR;
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ERROR);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ERROR);

    int cx = SCREEN_WIDTH / 2;
    drawCenteredText("WiFi Failed", cx, 130, 5, COL_ERROR, COL_BG);
    if (ssid.length() > 0) {
        drawCenteredText("SSID: " + ssid, cx, 210, 2, COL_DIM, COL_BG);
    }
    drawCenteredText("Retrying...", cx, 270, 3, COL_FG, COL_BG);
    drawCenteredText("Hold reset button to reconfigure WiFi",
                     cx, 370, 2, COL_DIM, COL_BG);
}

void DisplayUI::showScreensaver() {
    // Idle screen — the branded splash (its "Tap screen to begin" suits this).
    _screen = Screen::SCREENSAVER;
    _gfx->draw16bitRGBBitmap(0, 0, SPLASH_RGB565, SPLASH_W, SPLASH_H);
}

void DisplayUI::showError(const String& message) {
    _screen = Screen::ERROR;
    _gfx->fillScreen(COL_BG);

    int cx = SCREEN_WIDTH / 2;
    int cy = 130;

    _gfx->fillCircle(cx, cy, 60, COL_ERROR);
    for (int i = -3; i <= 3; i++) {
        _gfx->drawLine(cx - 24 + i, cy - 24, cx + 24 + i, cy + 24, COL_BG);
        _gfx->drawLine(cx + 24 + i, cy - 24, cx - 24 + i, cy + 24, COL_BG);
    }

    drawCenteredText("ERROR", cx, 240, 5, COL_ERROR, COL_BG);

    // Word-wrap to up to 2 lines (use the same font we'll draw with)
    applyPosFont(_gfx, 2);
    int16_t x1, y1; uint16_t w, h;
    _gfx->getTextBounds(message.c_str(), 0, 0, &x1, &y1, &w, &h);
    if ((int)w > SCREEN_WIDTH - 80) {
        int half = message.length() / 2;
        int sp = message.indexOf(' ', half);
        if (sp < 0) sp = half;
        drawCenteredText(message.substring(0, sp),     cx, 310, 2, COL_FG, COL_BG);
        drawCenteredText(message.substring(sp + 1),    cx, 345, 2, COL_FG, COL_BG);
    } else {
        drawCenteredText(message, cx, 320, 2, COL_FG, COL_BG);
    }

    drawCenteredText("Tap to retry", cx, SCREEN_HEIGHT - 28, 2, COL_DIM, COL_BG);
}

// ============================================================
// Transaction history
// ============================================================

// Left/right aligned text helpers (the on-screen tables need alignment that
// drawCenteredText doesn't give). yTop is the visual top of the glyphs.
static void drawLeftAt(Arduino_GFX* g, const String& s, int x, int yTop,
                       int size, uint16_t fg, uint16_t bg) {
    applyPosFont(g, size);
    g->setTextColor(fg, bg);
    int16_t x1, y1; uint16_t w, h;
    g->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
    g->setCursor(x - x1, yTop - y1);
    g->print(s);
}

static void drawRightAt(Arduino_GFX* g, const String& s, int xRight, int yTop,
                        int size, uint16_t fg, uint16_t bg) {
    applyPosFont(g, size);
    g->setTextColor(fg, bg);
    int16_t x1, y1; uint16_t w, h;
    g->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
    g->setCursor(xRight - w - x1, yTop - y1);
    g->print(s);
}

// "12,345" — group an integer with thousands separators.
static String groupInt(uint64_t n) {
    String raw;
    if (n == 0) raw = "0";
    while (n > 0) { raw = char('0' + (int)(n % 10)) + raw; n /= 10; }
    String out; int cnt = 0;
    for (int j = (int)raw.length() - 1; j >= 0; j--) {
        out = String(raw[j]) + out;
        if (++cnt % 3 == 0 && j > 0) out = "," + out;
    }
    return out;
}

// Rows of transactions that fit in the list area.
static int historyRowsPerPage() { return (TXL_BOTTOM - TXL_TOP) / TXL_ROW_H; }

// Gather indices of d.all whose createdAt is in the selected timeframe.
// d.all is already newest-first, so the result preserves that order.
static void historyMatches(const HistoryData& d, Timeframe tf,
                           std::vector<int>& out) {
    time_t s, e;
    timeframeRange(d, tf, s, e);
    out.clear();
    for (int i = 0; i < (int)d.all.size(); i++) {
        time_t t = d.all[i].createdAt;
        if (t >= s && t < e) out.push_back(i);
    }
}

void DisplayUI::resetHistoryView() {
    _histTf   = Timeframe::DAY;
    _histPage = 0;
}

void DisplayUI::showTransactionHistory(const HistoryData& d,
                                       const String& currency) {
    _screen = Screen::TXN_HISTORY;
    _gfx->fillScreen(COL_BG);
    drawHeader("Transactions");

    // Back button (top-left, over the orange header).
    _gfx->fillRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 7, COL_BG);
    drawCenteredText("< Back", BACK_X + BACK_W / 2, BACK_Y + BACK_H / 2, 2,
                     COL_ACCENT, COL_BG);

    if (!d.ok) {
        String msg = d.error == "Clock not set" ? "Clock not synced yet" : d.error;
        drawCenteredText("Couldn't load", SCREEN_WIDTH / 2, 360, 4, COL_ERROR, COL_BG);
        drawCenteredText(msg, SCREEN_WIDTH / 2, 430, 2, COL_DIM, COL_BG);
        return;
    }

    // --- Timeframe tabs ---
    for (int i = 0; i < TAB_COUNT; i++) {
        bool sel = ((int)_histTf == i);
        int x = TAB_X(i);
        _gfx->fillRoundRect(x, TAB_Y, TAB_W, TAB_H, 8,
                            sel ? COL_ACCENT : COL_KEYPAD_BG);
        drawCenteredText(timeframeLabel((Timeframe)i), x + TAB_W / 2,
                         TAB_Y + TAB_H / 2, 2, sel ? COL_BG : COL_FG,
                         sel ? COL_ACCENT : COL_KEYPAD_BG);
    }

    // --- Matches for the selected timeframe ---
    std::vector<int> idx;
    historyMatches(d, _histTf, idx);
    int count = (int)idx.size();

    int paidCount = 0;
    double paidNzd = 0;
    for (int i : idx) if (d.all[i].isPaid) { paidCount++; paidNzd += d.all[i].nzdAmount; }

    // Summary line: how many taken + paid total for the period.
    char summary[64];
    snprintf(summary, sizeof(summary), "%d txns  -  %d paid  -  $%.2f %s",
             count, paidCount, paidNzd, currency.c_str());
    drawCenteredText(summary, SCREEN_WIDTH / 2, 104, 2, COL_ACCENT, COL_BG);
    _gfx->drawFastHLine(TXL_XL, 118, SCREEN_WIDTH - 2 * TXL_XL, COL_DIM);

    // --- Clamp page + slice ---
    int rpp = historyRowsPerPage();
    int totalPages = count > 0 ? (count + rpp - 1) / rpp : 1;
    if (_histPage >= totalPages) _histPage = totalPages - 1;
    if (_histPage < 0) _histPage = 0;
    int first = _histPage * rpp;
    int last  = first + rpp; if (last > count) last = count;

    if (count == 0) {
        drawCenteredText("No transactions", SCREEN_WIDTH / 2, TXL_TOP + 40, 2,
                         COL_DIM, COL_BG);
        if (d.truncated)
            drawCenteredText("(older records omitted)", SCREEN_WIDTH / 2,
                             TXL_TOP + 80, 1, COL_DIM, COL_BG);
        return;
    }

    // --- Transaction rows ---
    for (int n = first; n < last; n++) {
        const TxRecord& r = d.all[idx[n]];
        int ry = TXL_TOP + (n - first) * TXL_ROW_H;

        char when[24] = "--";
        if (r.createdAt) {
            struct tm lt; localtime_r(&r.createdAt, &lt);
            strftime(when, sizeof(when), "%a %d %b %H:%M", &lt);
        }
        char money[16];
        snprintf(money, sizeof(money), "$%.2f", r.nzdAmount);

        // Single wide line: date | amount | sats | paid state | Check button.
        drawLeftAt (_gfx, when,  TXL_XL, ry + 17, 2, COL_FG,     COL_BG);
        drawRightAt(_gfx, money, 326,    ry + 17, 2, COL_ACCENT, COL_BG);
        drawRightAt(_gfx, groupInt(r.satAmount) + " sats",
                                 500,    ry + 19, 1, COL_DIM,    COL_BG);
        if (r.isPaid)
            drawRightAt(_gfx, "paid",   TXL_XR2, ry + 17, 2, COL_SUCCESS, COL_BG);
        else
            drawRightAt(_gfx, "unpaid", TXL_XR2, ry + 17, 2, COL_DIM,     COL_BG);

        drawCheckButton(ry + CHK_YOFF, "Check", COL_KEYPAD_BG, COL_FG);

        if (n < last - 1)
            _gfx->drawFastHLine(TXL_XL, ry + TXL_ROW_H - 1,
                                SCREEN_WIDTH - 2 * TXL_XL, 0x1082);
    }

    // --- Scroll footer ---
    char pageInfo[40];
    snprintf(pageInfo, sizeof(pageInfo), "%d-%d of %d", first + 1, last, count);
    drawCenteredText(pageInfo, SCREEN_WIDTH / 2, SCR_Y + SCR_H / 2, 2,
                     COL_FG, COL_BG);

    bool canPrev = _histPage > 0;
    bool canNext = _histPage < totalPages - 1;
    _gfx->fillRoundRect(SCR_PREV_X, SCR_Y, SCR_BTN_W, SCR_H, 8,
                        canPrev ? COL_KEYPAD_BG : 0x1082);
    drawCenteredText("Prev", SCR_PREV_X + SCR_BTN_W / 2, SCR_Y + SCR_H / 2, 2,
                     canPrev ? COL_FG : COL_DIM, canPrev ? COL_KEYPAD_BG : 0x1082);
    _gfx->fillRoundRect(SCR_NEXT_X, SCR_Y, SCR_BTN_W, SCR_H, 8,
                        canNext ? COL_KEYPAD_BG : 0x1082);
    drawCenteredText("Next", SCR_NEXT_X + SCR_BTN_W / 2, SCR_Y + SCR_H / 2, 2,
                     canNext ? COL_FG : COL_DIM, canNext ? COL_KEYPAD_BG : 0x1082);
}

void DisplayUI::drawCheckButton(int y, const String& label, uint16_t bg, uint16_t fg) {
    _gfx->fillRoundRect(CHK_X, y, CHK_W, CHK_H, 8, bg);
    drawCenteredText(label, CHK_X + CHK_W / 2, y + CHK_H / 2, 2, fg, bg);
}

void DisplayUI::showCheckResult(InvoiceState state) {
    if (_checkBtnY < 0) return;
    const char* label; uint16_t bg, fg;
    switch (state) {
        case InvoiceState::PAID:    label = "Paid";    bg = COL_SUCCESS;    fg = COL_BG; break;
        case InvoiceState::PENDING: label = "Pending"; bg = COL_ACCENT;     fg = COL_BG; break;
        case InvoiceState::EXPIRED: label = "Expired"; bg = COL_ERROR;      fg = COL_FG; break;
        default:                    label = "Error";   bg = COL_KEYPAD_BG;  fg = COL_ERROR; break;
    }
    drawCheckButton(_checkBtnY, label, bg, fg);
}

DisplayUI::HistEvent DisplayUI::pollTransactionHistory(const HistoryData& d,
                                                       const String& currency,
                                                       int& outRecordIdx) {
    uint16_t tx, ty;
    bool touched = gt911ReadTouch(&tx, &ty);
    if (!touched) { _wasTouched = false; return HistEvent::NONE; }
    if (_wasTouched || (millis() - _lastTouch <= 200)) return HistEvent::NONE;
    _wasTouched = true;
    _lastTouch  = millis();

    // Back button (top-left corner) — extend down toward, but not into, the tabs.
    if (tx < BACK_X + BACK_W + 14 && ty < BACK_Y + BACK_H + 8)
        return HistEvent::BACK;

    // Timeframe tabs — a little slop below, but clear of the summary line.
    if (ty >= TAB_Y && ty < TAB_Y + TAB_H + 6) {
        for (int i = 0; i < TAB_COUNT; i++) {
            int x = TAB_X(i);
            if (tx >= x && tx < x + TAB_W) {
                if ((int)_histTf != i) {
                    _histTf   = (Timeframe)i;
                    _histPage = 0;
                    showTransactionHistory(d, currency);
                }
                return HistEvent::NONE;
            }
        }
    }

    // Scroll buttons — extend down to the screen edge (rows sit just above).
    if (ty >= SCR_Y && ty < SCR_Y + SCR_H + TPAD_DN) {
        if (tx >= SCR_PREV_X && tx < SCR_PREV_X + SCR_BTN_W && _histPage > 0) {
            _histPage--;
            showTransactionHistory(d, currency);
        } else if (tx >= SCR_NEXT_X && tx < SCR_NEXT_X + SCR_BTN_W) {
            _histPage++;   // showTransactionHistory clamps to the last page
            showTransactionHistory(d, currency);
        }
        return HistEvent::NONE;
    }

    // A row's Check button.
    if (tx >= CHK_X && tx < CHK_X + CHK_W && ty >= TXL_TOP && ty < TXL_BOTTOM) {
        int row = (ty - TXL_TOP) / TXL_ROW_H;
        int ry  = TXL_TOP + row * TXL_ROW_H;
        if (ty < ry + CHK_YOFF || ty >= ry + CHK_YOFF + CHK_H)
            return HistEvent::NONE;   // tap fell in the inter-row gap

        std::vector<int> idx;
        historyMatches(d, _histTf, idx);
        int rpp = historyRowsPerPage();
        int n   = _histPage * rpp + row;
        if (n < 0 || n >= (int)idx.size()) return HistEvent::NONE;

        outRecordIdx = idx[n];
        _checkBtnY   = ry + CHK_YOFF;
        drawCheckButton(_checkBtnY, "...", COL_ACCENT, COL_BG);  // spinner
        return HistEvent::CHECK;
    }

    return HistEvent::NONE;
}

// ============================================================
// Touch
// ============================================================
Key DisplayUI::pollTouch() {
    uint16_t tx, ty;
    bool touched = gt911ReadTouch(&tx, &ty);

    if (touched && !_wasTouched && (millis() - _lastTouch > 200)) {
        _wasTouched = true;
        _lastTouch = millis();
        Serial.printf("[TOUCH] raw=%ld,%ld  scr=%u,%u\n",
                      (long)gt911LastRawX(), (long)gt911LastRawY(), tx, ty);
        if (_screen == Screen::AMOUNT_ENTRY) {
            // Whole top-right of the header is the menu hit area, but it must
            // stay inside the header band — the keypad sits right below it.
            if (tx >= MENU_X - 12 && ty < HDR_H) {
                return Key::MENU;
            }
            Key k = hitTest(tx, ty);
            Serial.printf("[TOUCH] hit -> key=%d\n", (int)k);
            return k;
        }
        if (_screen == Screen::SETUP_INFO) {
            if (tx >= TP_X && tx < TP_X + TP_W &&
                ty >= TP_Y && ty < TP_Y + TP_H) {
                return Key::TEST_PRINT;
            }
        }
        if (_screen == Screen::QR_DISPLAY) {
            if (tx >= CANCEL_X - 14 && tx < CANCEL_X + CANCEL_W + 14 &&
                ty >= CANCEL_Y - TPAD_UP && ty < CANCEL_Y + CANCEL_H + TPAD_DN) {
                return Key::CANCEL;
            }
        }
    }
    if (!touched) _wasTouched = false;
    return Key::NONE;
}

bool DisplayUI::anyTouch() {
    uint16_t tx, ty;
    bool touched = gt911ReadTouch(&tx, &ty);
    if (touched) {
        _gfx->fillCircle(tx, ty, 8, COL_ERROR);
    }
    if (touched && !_wasTouched && (millis() - _lastTouch > 300)) {
        _wasTouched = true;
        _lastTouch = millis();
        return true;
    }
    if (!touched) _wasTouched = false;
    return false;
}

// ============================================================
// BTCPay store picker — 2-column grid for landscape
// ============================================================
#define SS_COLS 2
#define SS_ROWS 3
#define SS_MAX  (SS_COLS * SS_ROWS)
#define SS_X0   20
#define SS_Y0   86
#define SS_W    372
#define SS_H    100
#define SS_GX   16
#define SS_GY   14
#define SS_CX(i) (SS_X0 + ((i) % SS_COLS) * (SS_W + SS_GX))
#define SS_CY(i) (SS_Y0 + ((i) / SS_COLS) * (SS_H + SS_GY))

int DisplayUI::storeSelectCapacity() { return SS_MAX; }

void DisplayUI::showStoreSelect(const String* names, int count) {
    _screen = Screen::STORE_SELECT;
    _gfx->fillScreen(COL_BG);
    drawHeader("Select Store");

    int shown = count < SS_MAX ? count : SS_MAX;
    for (int i = 0; i < shown; i++) {
        int x = SS_CX(i), y = SS_CY(i);
        _gfx->fillRoundRect(x, y, SS_W, SS_H, 10, COL_KEYPAD_BG);

        // Truncate long names so they don't overrun the cell.
        String label = names[i];
        if (label.length() > 18) label = label.substring(0, 17) + "…";
        drawCenteredText(label, x + SS_W / 2, y + SS_H / 2, 3,
                         COL_FG, COL_KEYPAD_BG);
    }

    drawCenteredText("Tap your store", SCREEN_WIDTH / 2,
                     SCREEN_HEIGHT - 26, 2, COL_DIM, COL_BG);
}

int DisplayUI::pollStoreSelect(int count) {
    uint16_t tx, ty;
    bool touched = gt911ReadTouch(&tx, &ty);

    if (touched && !_wasTouched && (millis() - _lastTouch > 250)) {
        _wasTouched = true;
        _lastTouch = millis();
        int shown = count < SS_MAX ? count : SS_MAX;
        for (int i = 0; i < shown; i++) {
            int x = SS_CX(i), y = SS_CY(i);
            if (tx >= x && tx < x + SS_W && ty >= y && ty < y + SS_H) {
                Serial.printf("[TOUCH] store cell %d\n", i);
                return i;
            }
        }
    }
    if (!touched) _wasTouched = false;
    return -1;
}

Key DisplayUI::hitTest(int tx, int ty) {
    if (ty < KP_Y0 || tx < KP_X0) return Key::NONE;
    int r = (ty - KP_Y0) / (KP_BH + KP_GAP);
    int c = (tx - KP_X0) / (KP_BW + KP_GAP);
    if (r < 0 || r >= KP_ROWS || c < 0 || c >= KP_COLS) return Key::NONE;
    return keyMap(r, c);
}

// ============================================================
// QR rendering
// ============================================================
void DisplayUI::drawQRCode(const String& data, int x, int y, int areaSize) {
    QRCode qr;
    uint8_t buf[qrcode_getBufferSize(QR_VERSION)];
    qrcode_initText(&qr, buf, QR_VERSION, QR_ECC_LEVEL, data.c_str());

    int modPx = areaSize / qr.size;
    int totalPx = modPx * qr.size;
    int ox = x + (areaSize - totalPx) / 2;
    int oy = y + (areaSize - totalPx) / 2;

    _gfx->fillRect(x - 8, y - 8, areaSize + 16, areaSize + 16, 0xFFFF);

    for (uint8_t qy = 0; qy < qr.size; qy++)
        for (uint8_t qx = 0; qx < qr.size; qx++)
            if (qrcode_getModule(&qr, qx, qy))
                _gfx->fillRect(ox + qx * modPx, oy + qy * modPx, modPx, modPx, 0x0000);
}
