#include "display_ui.h"
#include "money_fmt.h"
#include "config.h"
#include "setup_portal.h"
#include "logo.h"
#include "splash_image.h"
#include "pos_fonts.h"
#include <qrcode.h>

// ============================================================
// Layout constants — 480x800 portrait
// ============================================================
#define HDR_H      50
#define AMT_BOX_Y  70
#define AMT_BOX_H  90
#define KP_X0      15
#define KP_Y0      190
#define KP_COLS    4
#define KP_ROWS    4
#define KP_BW      110
#define KP_BH      130
#define KP_GAP     7

// Test Print button on the SETUP_INFO screen
#define TP_X       60
#define TP_Y       700
#define TP_W       360
#define TP_H       70

// Menu button in the top-right of the amount-entry header (opens history)
#define MENU_W     54
#define MENU_H     38
#define MENU_X     (SCREEN_WIDTH - MENU_W - 8)
#define MENU_Y     ((HDR_H - MENU_H) / 2)

// Cancel button on the invoice (QR) screen — the only way to cancel a sale,
// so a mistap elsewhere on the screen no longer closes it. Sits below the
// timer band (updateTimer only clears y 658..732).
#define CANCEL_W   220
#define CANCEL_H   52
#define CANCEL_X   ((SCREEN_WIDTH - CANCEL_W) / 2)
#define CANCEL_Y   742

// Extra touch slop for the small buttons that sit in the screen's top/bottom
// extremes, beyond the touch-calibration range. The hit area extends past the
// drawn button (mostly downward) so the whole button is comfortably tappable.
#define TPAD_UP    14
#define TPAD_DN    24

// --- Transaction history screen ---
// Back button (top-left of the header)
#define BACK_W     78
#define BACK_H     38
#define BACK_X     6
#define BACK_Y     ((HDR_H - BACK_H) / 2)

// Firmware-update button — mirrors Back on the right edge of the header.
#define UPD_W      100
#define UPD_X      (SCREEN_WIDTH - UPD_W - 6)

// Timeframe tabs (4 across, just under the header)
#define TAB_Y      54
#define TAB_H      44
#define TAB_MARGIN 8
#define TAB_GAP    6
#define TAB_W      ((SCREEN_WIDTH - 2 * TAB_MARGIN - 3 * TAB_GAP) / 4)
#define TAB_COUNT  4
#define TAB_X(i)   (TAB_MARGIN + (i) * (TAB_W + TAB_GAP))

// Transaction list area
#define TXL_TOP    140
#define TXL_BOTTOM 744
#define TXL_ROW_H  58
#define TXL_XL     18
#define TXL_XR     (SCREEN_WIDTH - 18)

// Per-row "Check" button (re-checks live invoice status)
#define CHK_W      98
#define CHK_H      40
#define CHK_X      (SCREEN_WIDTH - 18 - CHK_W)
#define CHK_YOFF   ((TXL_ROW_H - CHK_H) / 2)   // y within a row
// Left content stops just before the Check button.
#define TXL_XR2    (CHK_X - 12)

// Scroll footer (prev / page indicator / next)
#define SCR_Y      750
#define SCR_H      44
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
            drawCenteredText("(tap the teal target)",
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

    auto& c = touchCalib();
    c.rawTLx = raw[0][0]; c.rawTLy = raw[0][1];
    c.rawBRx = raw[1][0]; c.rawBRy = raw[1][1];
    c.scrTLx = targets[0][0]; c.scrTLy = targets[0][1];
    c.scrBRx = targets[1][0]; c.scrBRy = targets[1][1];
    c.done = true;
    Serial.printf("[CAL] done TL=(%ld,%ld)->(%ld,%ld) BR=(%ld,%ld)->(%ld,%ld)\n",
                  (long)c.rawTLx, (long)c.rawTLy, (long)c.scrTLx, (long)c.scrTLy,
                  (long)c.rawBRx, (long)c.rawBRy, (long)c.scrBRx, (long)c.scrBRy);
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

// Hamburger menu button drawn over the right end of the teal header.
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
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ACCENT);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ACCENT);

    // Stacked logo centred on black.
    int logoX = (SCREEN_WIDTH - LOGO_W) / 2;
    int logoY = 310;
    _gfx->draw16bitRGBBitmap(logoX, logoY, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("Bitcoin Point of Sale", SCREEN_WIDTH / 2, 420, 2, COL_FG, COL_BG);

    if (merchantName.length() > 0) {
        drawCenteredText(merchantName, SCREEN_WIDTH / 2, 480, 2, COL_DIM, COL_BG);
    }
    drawCenteredText("Lightning Network", SCREEN_WIDTH / 2, 560, 2, COL_ACCENT, COL_BG);
}

void DisplayUI::showSetupInfo() {
    _screen = Screen::SETUP_INFO;
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ACCENT);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ACCENT);

    // Stacked logo at the top.
    _gfx->draw16bitRGBBitmap((SCREEN_WIDTH - LOGO_W) / 2, 30, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("POS SETUP", SCREEN_WIDTH / 2, 135, 4, COL_ACCENT, COL_BG);

    drawCenteredText("Connect to WiFi:", SCREEN_WIDTH / 2, 220, 2, COL_FG, COL_BG);
    drawCenteredText(SetupPortal::apSSID(), SCREEN_WIDTH / 2, 280, 3, COL_ACCENT, COL_BG);

    drawCenteredText("Then open browser to:", SCREEN_WIDTH / 2, 400, 2, COL_FG, COL_BG);
    drawCenteredText(SETUP_AP_IP,             SCREEN_WIDTH / 2, 460, 3, COL_ACCENT, COL_BG);

    // NFC diagnostic band (overwritten by showNfcTap on each card tap).
    drawCenteredText("Tap NFC card to test", SCREEN_WIDTH / 2, 537, 2, COL_DIM, COL_BG);

    drawCenteredText("Enter WiFi details and",   SCREEN_WIDTH / 2, 600, 2, COL_DIM, COL_BG);
    drawCenteredText("your payment provider",    SCREEN_WIDTH / 2, 640, 2, COL_DIM, COL_BG);

    // Test Print button — exercise the thermal printer without a real sale
    _gfx->fillRoundRect(TP_X, TP_Y, TP_W, TP_H, 10, COL_KEYPAD_BG);
    _gfx->drawRoundRect(TP_X, TP_Y, TP_W, TP_H, 10, COL_ACCENT);
    drawCenteredText("Test Print", SCREEN_WIDTH / 2, TP_Y + TP_H / 2,
                     3, COL_ACCENT, COL_KEYPAD_BG);
}

// Diagnostic readout for NFC bench testing: fills the band between the setup
// screen's browser-IP line and the instructions with the last tap's UID (and
// a truncated NDEF URL), so a reader can be tested without a serial monitor.
void DisplayUI::showNfcTap(const String& uid, const String& url, int count) {
    const int top = 488, h = 100;
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
        drawHeader("Stacked Bitcoin");
        drawMenuButton();
    }

    // Amount box (redraws the box fill → wipes the previous amount text)
    _gfx->fillRoundRect(15, AMT_BOX_Y, SCREEN_WIDTH - 30, AMT_BOX_H, 8, COL_KEYPAD_BG);
    _gfx->drawRoundRect(15, AMT_BOX_Y, SCREEN_WIDTH - 30, AMT_BOX_H, 8, COL_ACCENT);

    const bool satsMode = (currency == "SATS");
    String disp = amount.isEmpty() ? (satsMode ? "0" : "0.00") : amount;
    String right = disp + " " + currency;

    // GFXfont baseline-positioning: use getTextBounds + (x1,y1) so "$" and
    // the amount string sit visually centred inside the rounded box.
    int boxCy = AMT_BOX_Y + AMT_BOX_H / 2;
    int16_t x1, y1; uint16_t w, h;

    applyPosFont(_gfx, 4);
    const char* prefix = currencyPrefix(currency);   // "" for SATS / EUR / …
    if (*prefix) {
        _gfx->setTextColor(COL_ACCENT, COL_KEYPAD_BG);
        _gfx->getTextBounds(prefix, 0, 0, &x1, &y1, &w, &h);
        _gfx->setCursor(30 - x1, boxCy - h / 2 - y1);
        _gfx->print(prefix);
    }

    _gfx->setTextColor(COL_FG, COL_KEYPAD_BG);
    _gfx->getTextBounds(right.c_str(), 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(SCREEN_WIDTH - 30 - w - x1, boxCy - h / 2 - y1);
    _gfx->print(right);

    // Keypad — static between keystrokes, only paint on first entry.
    if (!alreadyHere) {
        for (int r = 0; r < KP_ROWS; r++) {
            for (int c = 0; c < KP_COLS; c++) {
                const char* lbl = KP[r][c];
                if (strlen(lbl) == 0) continue;
                if (satsMode && strcmp(lbl, ".") == 0) continue;   // whole sats only

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
    drawHeader("Stacked Bitcoin");
    drawCenteredText(message, SCREEN_WIDTH / 2, 380, 3, COL_FG, COL_BG);
    drawCenteredText(". . .", SCREEN_WIDTH / 2, 450, 3, COL_ACCENT, COL_BG);
}

void DisplayUI::showQR(const String& bolt11, uint64_t sats, float nzd,
                       int secsLeft, int refreshCount, const String& currency) {
    _screen = Screen::QR_DISPLAY;
    _gfx->fillScreen(COL_BG);

    // Amount at top: fiat, or the sats figure itself in sats mode.
    if (nzd > 0 || currency == "SATS") {
        drawCenteredText(formatAmount(nzd, sats, currency),
                         SCREEN_WIDTH / 2, 30, 4, COL_ACCENT, COL_BG);
    }

    // QR in middle
    int qrSize = 440;
    int qrX = (SCREEN_WIDTH - qrSize) / 2;
    int qrY = 80;
    String qrData = bolt11;
    qrData.toUpperCase();
    drawQRCode(qrData, qrX, qrY, qrSize);

    // Info under QR (already the headline figure in sats mode)
    if (currency != "SATS") {
        char satStr[32]; snprintf(satStr, sizeof(satStr), "%lu sats", (unsigned long)sats);
        drawCenteredText(satStr, SCREEN_WIDTH / 2, 560, 2, COL_FG, COL_BG);
    }

    _gfx->drawFastHLine(30, 600, SCREEN_WIDTH - 60, COL_DIM);

    drawCenteredText("Scan with any Lightning wallet",
                     SCREEN_WIDTH / 2, 625, 2, COL_FG, COL_BG);

    updateTimer(secsLeft, refreshCount);

    // Cancel button (only this cancels — mistaps elsewhere are ignored).
    _gfx->fillRoundRect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 10, COL_KEYPAD_BG);
    _gfx->drawRoundRect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 10, COL_ERROR);
    drawCenteredText("Cancel", SCREEN_WIDTH / 2, CANCEL_Y + CANCEL_H / 2, 3,
                     COL_ERROR, COL_KEYPAD_BG);
}

void DisplayUI::updateTimer(int secsLeft, int refreshCount) {
    // Clear only down to 732 so the Cancel button (y 742+) is never touched.
    _gfx->fillRect(0, 658, SCREEN_WIDTH, 74, COL_BG);

    // ":45" under a minute, "4:59" above (self-custody invoices run longer).
    char t[12];
    if (secsLeft >= 60) snprintf(t, sizeof(t), "%d:%02d", secsLeft / 60, secsLeft % 60);
    else                snprintf(t, sizeof(t), ":%02d", secsLeft);
    drawCenteredText(t, SCREEN_WIDTH / 2, 684, 4,
                     secsLeft < 10 ? COL_ERROR : COL_FG, COL_BG);

    if (refreshCount > 0) {
        char r[32];
        snprintf(r, sizeof(r), "refresh %d/%d", refreshCount, MAX_INVOICE_REFRESHES);
        drawCenteredText(r, SCREEN_WIDTH / 2, 716, 2, COL_DIM, COL_BG);
    }
}

void DisplayUI::showPaid(uint64_t sats, float nzd, const String& currency) {
    _screen = Screen::PAID;
    _gfx->fillScreen(COL_BG);

    int cx = SCREEN_WIDTH / 2;

    int logoX = (SCREEN_WIDTH - LOGO_W) / 2;
    int logoY = 60;
    _gfx->draw16bitRGBBitmap(logoX, logoY, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("PAID", cx, 380, 7, COL_ACCENT, COL_BG);

    const bool fiat = (nzd > 0 && currency != "SATS");
    drawCenteredText(fiat ? formatAmount(nzd, sats, currency)
                          : formatAmount(0, sats, "SATS"),
                     cx, 490, 3, COL_FG, COL_BG);
    if (fiat) {
        char s[32]; snprintf(s, sizeof(s), "%lu sats", (unsigned long)sats);
        drawCenteredText(s, cx, 540, 2, COL_DIM, COL_BG);
    }

    drawCenteredText("Tap to continue", cx, SCREEN_HEIGHT - 30, 2, COL_DIM, COL_BG);
}

void DisplayUI::showWifiError(const String& ssid) {
    _screen = Screen::ERROR;
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ERROR);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ERROR);

    int cx = SCREEN_WIDTH / 2;
    drawCenteredText("WiFi Failed", cx, 220, 5, COL_ERROR, COL_BG);
    if (ssid.length() > 0) {
        drawCenteredText("SSID: " + ssid, cx, 320, 2, COL_DIM, COL_BG);
    }
    drawCenteredText("Retrying...", cx, 400, 3, COL_FG, COL_BG);
    drawCenteredText("Hold reset button", cx, 540, 2, COL_DIM, COL_BG);
    drawCenteredText("to reconfigure WiFi", cx, 580, 2, COL_DIM, COL_BG);
}

void DisplayUI::showBootError(const String& title, const String& subject,
                              const String& reason, bool retrying) {
    _screen = Screen::ERROR;
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ERROR);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ERROR);

    int cx = SCREEN_WIDTH / 2;
    // This screen does its own line breaking; GFX auto-wrap would otherwise
    // fold an over-wide token onto the next row at the wrong pitch.
    _gfx->setTextWrap(false);
    int16_t x1, y1; uint16_t w, h;

    // Title: drop a size if it won't fit on one line ("Bad Lightning
    // Address" is 513 px at size 4 on a 480 px panel).
    applyPosFont(_gfx, 4);
    _gfx->getTextBounds(title.c_str(), 0, 0, &x1, &y1, &w, &h);
    drawCenteredText(title, cx, 150, ((int)w > SCREEN_WIDTH - 20) ? 3 : 4,
                     COL_ERROR, COL_BG);

    // The configured address — the thing the merchant needs to read. Long
    // ones split at '@'; URL/bech32 forms get their middle elided.
    if (subject.length()) {
        applyPosFont(_gfx, 2);
        _gfx->getTextBounds(subject.c_str(), 0, 0, &x1, &y1, &w, &h);
        int at = subject.indexOf('@');
        if ((int)w <= SCREEN_WIDTH - 40) {
            drawCenteredText(subject, cx, 230, 2, COL_DIM, COL_BG);
        } else if (at > 0) {
            drawCenteredText(subject.substring(0, at + 1), cx, 215, 2, COL_DIM, COL_BG);
            drawCenteredText(subject.substring(at + 1),    cx, 247, 2, COL_DIM, COL_BG);
        } else {
            // Shrink the kept head/tail until the elided form fits one line
            // (upper-case bech32 is ~2x wider than lower-case).
            int keep = (int)subject.length() / 2 - 2;
            if (keep > 16) keep = 16;
            String shown;
            do {
                shown = subject.substring(0, keep) + "..." +
                        subject.substring(subject.length() - keep);
                _gfx->getTextBounds(shown.c_str(), 0, 0, &x1, &y1, &w, &h);
            } while ((int)w > SCREEN_WIDTH - 40 && --keep > 3);
            drawCenteredText(shown, cx, 230, 2, COL_DIM, COL_BG);
        }
    }

    // Reason, wrapped onto up to four lines at word boundaries (an
    // unbreakable over-wide token is hard-broken by character).
    applyPosFont(_gfx, 2);
    String rest = reason;
    int y = 300;
    for (int line = 0; line < 4 && rest.length(); line++) {
        String take = rest;
        _gfx->getTextBounds(take.c_str(), 0, 0, &x1, &y1, &w, &h);
        while ((int)w > SCREEN_WIDTH - 40) {
            int sp = take.lastIndexOf(' ');
            if (sp > 0) take = take.substring(0, sp);
            else if (take.length() > 1) take.remove(take.length() - 1);
            else break;
            _gfx->getTextBounds(take.c_str(), 0, 0, &x1, &y1, &w, &h);
        }
        drawCenteredText(take, cx, y, 2, COL_FG, COL_BG);
        rest = rest.substring(take.length());
        rest.trim();
        y += 36;
    }

    drawCenteredText(retrying ? "Retrying..." : "This wallet can't be used",
                     cx, 470, 3, retrying ? COL_FG : COL_ERROR, COL_BG);

    drawCenteredText("Tap screen: re-enter setup", cx, 600, 2, COL_ACCENT, COL_BG);
    drawCenteredText("Hold BOOT 5 s: factory reset", cx, 640, 2, COL_DIM, COL_BG);
    _gfx->setTextWrap(true);   // other screens rely on the default
}

void DisplayUI::showScreensaver() {
    _screen = Screen::SCREENSAVER;
    _gfx->draw16bitRGBBitmap(0, 0, SPLASH_RGB565, SPLASH_W, SPLASH_H);
}

void DisplayUI::showError(const String& message) {
    _screen = Screen::ERROR;
    _gfx->fillScreen(COL_BG);

    int cx = SCREEN_WIDTH / 2;
    int cy = 230;

    _gfx->fillCircle(cx, cy, 70, COL_ERROR);
    for (int i = -3; i <= 3; i++) {
        _gfx->drawLine(cx - 26 + i, cy - 26, cx + 26 + i, cy + 26, COL_BG);
        _gfx->drawLine(cx + 26 + i, cy - 26, cx - 26 + i, cy + 26, COL_BG);
    }

    drawCenteredText("ERROR", cx, 370, 5, COL_ERROR, COL_BG);

    // Word-wrap to up to 3 lines (use the same font we'll draw with)
    applyPosFont(_gfx, 2);
    int16_t x1, y1; uint16_t w, h;
    _gfx->getTextBounds(message.c_str(), 0, 0, &x1, &y1, &w, &h);
    if ((int)w > SCREEN_WIDTH - 40) {
        int len = message.length();
        int third = len / 3;
        int sp1 = message.indexOf(' ', third);
        int sp2 = message.indexOf(' ', 2 * third);
        if (sp1 < 0) sp1 = third;
        if (sp2 < 0) sp2 = 2 * third;
        drawCenteredText(message.substring(0, sp1),        cx, 460, 2, COL_FG, COL_BG);
        drawCenteredText(message.substring(sp1 + 1, sp2),  cx, 495, 2, COL_FG, COL_BG);
        drawCenteredText(message.substring(sp2 + 1),       cx, 530, 2, COL_FG, COL_BG);
    } else {
        drawCenteredText(message, cx, 480, 2, COL_FG, COL_BG);
    }

    drawCenteredText("Tap to retry", cx, SCREEN_HEIGHT - 30, 2, COL_DIM, COL_BG);
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

    // Back button (top-left, over the teal header).
    _gfx->fillRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 7, COL_BG);
    drawCenteredText("< Back", BACK_X + BACK_W / 2, BACK_Y + BACK_H / 2, 2,
                     COL_ACCENT, COL_BG);

    // Firmware-update button (top-right) — opens the on-device update menu.
    _gfx->fillRoundRect(UPD_X, BACK_Y, UPD_W, BACK_H, 7, COL_BG);
    drawCenteredText("Update", UPD_X + UPD_W / 2, BACK_Y + BACK_H / 2, 2,
                     COL_ACCENT, COL_BG);

    if (!d.ok) {
        if (d.error.indexOf("not supported") >= 0) {
            // Not a fault — the provider simply has no history endpoint
            // (BTCPay, self-custody). Point at where the records live.
            drawCenteredText("No history here", SCREEN_WIDTH / 2, 360, 4, COL_DIM, COL_BG);
            drawCenteredText("See your wallet / server for sales",
                             SCREEN_WIDTH / 2, 430, 2, COL_DIM, COL_BG);
            return;
        }
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
    snprintf(summary, sizeof(summary), "%d txns  -  %d paid  -  %s%.2f %s",
             count, paidCount, currencyPrefix(currency), paidNzd, currency.c_str());
    drawCenteredText(summary, SCREEN_WIDTH / 2, 118, 2, COL_ACCENT, COL_BG);
    _gfx->drawFastHLine(TXL_XL, 134, SCREEN_WIDTH - 2 * TXL_XL, COL_DIM);

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
        snprintf(money, sizeof(money), "%s%.2f", currencyPrefix(currency), r.nzdAmount);

        // Left: date over amount. Right of that (before the button): the
        // recorded paid state + sats. Far right: the Check button.
        drawLeftAt(_gfx, when, TXL_XL, ry + 4, 2, COL_FG, COL_BG);
        if (r.isPaid)
            drawRightAt(_gfx, "paid", TXL_XR2, ry + 6, 1, COL_SUCCESS, COL_BG);
        else
            drawRightAt(_gfx, "unpaid", TXL_XR2, ry + 6, 1, COL_DIM, COL_BG);

        drawLeftAt(_gfx, money, TXL_XL, ry + 30, 2, COL_ACCENT, COL_BG);
        drawRightAt(_gfx, groupInt(r.satAmount) + " sats",
                    TXL_XR2, ry + 32, 1, COL_DIM, COL_BG);

        drawCheckButton(ry + CHK_YOFF, "Check", COL_KEYPAD_BG, COL_FG);

        if (n < last - 1)
            _gfx->drawFastHLine(TXL_XL, ry + TXL_ROW_H - 2,
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

    // Firmware-update button (top-right corner).
    if (tx >= UPD_X - 14 && ty < BACK_Y + BACK_H + 8)
        return HistEvent::UPDATE;

    // Timeframe tabs — a little slop below (the summary line is non-interactive).
    if (ty >= TAB_Y && ty < TAB_Y + TAB_H + 16) {
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
            // Generous hit area: the button is small and above the touch
            // calibration range, and the whole top-right header corner is
            // otherwise dead space (amount box starts at y=70).
            if (tx >= MENU_X - 14 && ty < MENU_Y + MENU_H + TPAD_DN) {
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

bool DisplayUI::touchPoint(uint16_t& x, uint16_t& y) {
    bool touched = gt911ReadTouch(&x, &y);
    if (touched && !_wasTouched && (millis() - _lastTouch > 250)) {
        _wasTouched = true;
        _lastTouch = millis();
        return true;
    }
    if (!touched) _wasTouched = false;
    return false;
}

// ============================================================
// BTCPay store picker
// ============================================================
#define SS_Y0   90
#define SS_X    20
#define SS_W    (SCREEN_WIDTH - 40)
#define SS_H    90
#define SS_GAP  14
#define SS_MAX  6   // rows that fit between header and footer

int DisplayUI::storeSelectCapacity() { return SS_MAX; }

void DisplayUI::showStoreSelect(const String* names, int count) {
    _screen = Screen::STORE_SELECT;
    _gfx->fillScreen(COL_BG);
    drawHeader("Select Store");

    int shown = count < SS_MAX ? count : SS_MAX;
    for (int i = 0; i < shown; i++) {
        int y = SS_Y0 + i * (SS_H + SS_GAP);
        _gfx->fillRoundRect(SS_X, y, SS_W, SS_H, 10, COL_KEYPAD_BG);

        // Truncate long names so they don't overrun the row.
        String label = names[i];
        if (label.length() > 18) label = label.substring(0, 17) + "…";
        drawCenteredText(label, SCREEN_WIDTH / 2, y + SS_H / 2, 3,
                         COL_FG, COL_KEYPAD_BG);
    }

    drawCenteredText("Tap your store", SCREEN_WIDTH / 2,
                     SCREEN_HEIGHT - 28, 2, COL_DIM, COL_BG);
}

int DisplayUI::pollStoreSelect(int count) {
    uint16_t tx, ty;
    bool touched = gt911ReadTouch(&tx, &ty);

    if (touched && !_wasTouched && (millis() - _lastTouch > 250)) {
        _wasTouched = true;
        _lastTouch = millis();
        int shown = count < SS_MAX ? count : SS_MAX;
        for (int i = 0; i < shown; i++) {
            int y = SS_Y0 + i * (SS_H + SS_GAP);
            if (tx >= SS_X && tx < SS_X + SS_W && ty >= y && ty < y + SS_H) {
                Serial.printf("[TOUCH] store row %d\n", i);
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
// Alphanumeric-mode capacity at ECC L for QR versions 1..40 (ISO 18004
// table 7). Used to pick the smallest version that holds the invoice.
static const uint16_t QR_ALNUM_CAP_L[40] = {
      25,   47,   77,  114,  154,  195,  224,  279,  335,  395,
     468,  535,  619,  667,  758,  854,  938, 1046, 1153, 1249,
    1352, 1460, 1588, 1704, 1853, 1990, 2132, 2223, 2369, 2520,
    2677, 2840, 3009, 3183, 3351, 3537, 3729, 3927, 4087, 4296 };

// The capacity table only holds for QR alphanumeric mode (0-9 A-Z and
// " $%*+-./:"). Anything else makes the encoder fall back to byte mode
// with much less room — and this library has NO overflow check (its
// qrcode.c literally says "@TODO: Return error if data is too big"), so
// oversize input silently produces an unscannable QR or scribbles past a
// stack buffer. Every gate therefore has to happen before it's called.
static bool isQrAlphanumeric(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            strchr(" $%*+-./:", c)) continue;
        return false;
    }
    return true;
}

static_assert(QR_MAX_VERSION >= QR_VERSION && QR_MAX_VERSION <= 40, "QR version range");
static_assert(QR_MAX_CHARS == 1249, "QR_MAX_CHARS must match QR_ALNUM_CAP_L[QR_MAX_VERSION-1]");

bool DisplayUI::drawQRCode(const String& data, int x, int y, int areaSize) {
    _gfx->fillRect(x - 8, y - 8, areaSize + 16, areaSize + 16, 0xFFFF);

    // Smallest version in [QR_VERSION, QR_MAX_VERSION] with room for the
    // data. Decided here, up front — see the note above.
    int version = QR_VERSION;
    while (version < QR_MAX_VERSION &&
           data.length() > QR_ALNUM_CAP_L[version - 1]) version++;

    if (!isQrAlphanumeric(data) ||
        data.length() > QR_ALNUM_CAP_L[QR_MAX_VERSION - 1]) {
        Serial.printf("[QR] cannot encode %u chars (max version %d, alnum=%d)\n",
                      (unsigned)data.length(), QR_MAX_VERSION, isQrAlphanumeric(data));
        drawCenteredText("Invoice too long",  x + areaSize / 2, y + areaSize / 2 - 24, 3, COL_ERROR, 0xFFFF);
        drawCenteredText("to show as QR",     x + areaSize / 2, y + areaSize / 2 + 24, 3, COL_ERROR, 0xFFFF);
        return false;
    }

    QRCode qr;
    // qrcode_getBufferSize(v20) = 1177 bytes; the encoder adds ~3.3 KB more
    // of VLAs — main.cpp grows the loop task's stack to make room.
    uint8_t buf[qrcode_getBufferSize(QR_MAX_VERSION)];
    qrcode_initText(&qr, buf, version, QR_ECC_LEVEL, data.c_str());

    if (version != QR_VERSION) {
        Serial.printf("[QR] %u chars -> version %d (%d modules)\n",
                      (unsigned)data.length(), version, qr.size);
    }

    int modPx = areaSize / qr.size;
    int totalPx = modPx * qr.size;
    int ox = x + (areaSize - totalPx) / 2;
    int oy = y + (areaSize - totalPx) / 2;

    for (uint8_t qy = 0; qy < qr.size; qy++)
        for (uint8_t qx = 0; qx < qr.size; qx++)
            if (qrcode_getModule(&qr, qx, qy))
                _gfx->fillRect(ox + qx * modPx, oy + qy * modPx, modPx, modPx, 0x0000);
    return true;
}
