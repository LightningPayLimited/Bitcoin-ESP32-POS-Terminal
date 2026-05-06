#include "display_ui.h"
#include "config.h"
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

    int logoX = (SCREEN_WIDTH - LOGO_W) / 2;
    int logoY = 80;
    _gfx->draw16bitRGBBitmap(logoX, logoY, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("STACKED", SCREEN_WIDTH / 2, 380, 7, COL_ACCENT, COL_BG);
    drawCenteredText("Bitcoin Point of Sale", SCREEN_WIDTH / 2, 460, 2, COL_FG, COL_BG);

    if (merchantName.length() > 0) {
        drawCenteredText(merchantName, SCREEN_WIDTH / 2, 530, 2, COL_DIM, COL_BG);
    }
    drawCenteredText("Lightning Network", SCREEN_WIDTH / 2, 600, 2, COL_DIM, COL_BG);
}

void DisplayUI::showSetupInfo() {
    _screen = Screen::SETUP_INFO;
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 5, COL_ACCENT);
    _gfx->fillRect(0, SCREEN_HEIGHT - 5, SCREEN_WIDTH, 5, COL_ACCENT);

    drawCenteredText("POS SETUP", SCREEN_WIDTH / 2, 80, 4, COL_ACCENT, COL_BG);

    drawCenteredText("Connect to WiFi:", SCREEN_WIDTH / 2, 220, 2, COL_FG, COL_BG);
    drawCenteredText(SETUP_AP_SSID,       SCREEN_WIDTH / 2, 280, 3, COL_ACCENT, COL_BG);

    drawCenteredText("Then open browser to:", SCREEN_WIDTH / 2, 400, 2, COL_FG, COL_BG);
    drawCenteredText(SETUP_AP_IP,             SCREEN_WIDTH / 2, 460, 3, COL_ACCENT, COL_BG);

    drawCenteredText("Enter WiFi details",       SCREEN_WIDTH / 2, 600, 2, COL_DIM, COL_BG);
    drawCenteredText("and Stacked API key",      SCREEN_WIDTH / 2, 640, 2, COL_DIM, COL_BG);

    // Test Print button — exercise the thermal printer without a real sale
    _gfx->fillRoundRect(TP_X, TP_Y, TP_W, TP_H, 10, COL_KEYPAD_BG);
    _gfx->drawRoundRect(TP_X, TP_Y, TP_W, TP_H, 10, COL_ACCENT);
    drawCenteredText("Test Print", SCREEN_WIDTH / 2, TP_Y + TP_H / 2,
                     3, COL_ACCENT, COL_KEYPAD_BG);
}

void DisplayUI::showAmountEntry(const String& amount, const String& currency) {
    _screen = Screen::AMOUNT_ENTRY;
    _gfx->fillScreen(COL_BG);
    drawHeader("Stacked: Pay with Bitcoin");



    // Amount box
    _gfx->fillRoundRect(15, AMT_BOX_Y, SCREEN_WIDTH - 30, AMT_BOX_H, 8, COL_KEYPAD_BG);
    _gfx->drawRoundRect(15, AMT_BOX_Y, SCREEN_WIDTH - 30, AMT_BOX_H, 8, COL_ACCENT);

    String disp = amount.isEmpty() ? "0.00" : amount;
    String right = disp + " " + currency;

    // GFXfont baseline-positioning: use getTextBounds + (x1,y1) so "$" and
    // the amount string sit visually centred inside the rounded box.
    int boxCy = AMT_BOX_Y + AMT_BOX_H / 2;
    int16_t x1, y1; uint16_t w, h;

    applyPosFont(_gfx, 4);
    _gfx->setTextColor(COL_ACCENT, COL_KEYPAD_BG);
    _gfx->getTextBounds("$", 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(30 - x1, boxCy - h / 2 - y1);
    _gfx->print("$");

    _gfx->setTextColor(COL_FG, COL_KEYPAD_BG);
    _gfx->getTextBounds(right.c_str(), 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(SCREEN_WIDTH - 30 - w - x1, boxCy - h / 2 - y1);
    _gfx->print(right);

    // Keypad
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

void DisplayUI::showLoading(const String& message) {
    _screen = Screen::LOADING;
    _gfx->fillScreen(COL_BG);
    drawHeader("Stacked");
    drawCenteredText(message, SCREEN_WIDTH / 2, 380, 3, COL_FG, COL_BG);
    drawCenteredText(". . .", SCREEN_WIDTH / 2, 450, 3, COL_ACCENT, COL_BG);
}

void DisplayUI::showQR(const String& bolt11, uint64_t sats, float nzd,
                       int secsLeft, int refreshCount) {
    _screen = Screen::QR_DISPLAY;
    _gfx->fillScreen(COL_BG);

    // Amount at top
    if (nzd > 0) {
        char s[16]; snprintf(s, sizeof(s), "$%.2f NZD", nzd);
        drawCenteredText(s, SCREEN_WIDTH / 2, 30, 4, COL_ACCENT, COL_BG);
    }

    // QR in middle
    int qrSize = 440;
    int qrX = (SCREEN_WIDTH - qrSize) / 2;
    int qrY = 80;
    String qrData = bolt11;
    qrData.toUpperCase();
    drawQRCode(qrData, qrX, qrY, qrSize);

    // Info under QR
    char satStr[32]; snprintf(satStr, sizeof(satStr), "%lu sats", (unsigned long)sats);
    drawCenteredText(satStr, SCREEN_WIDTH / 2, 560, 2, COL_FG, COL_BG);

    _gfx->drawFastHLine(30, 600, SCREEN_WIDTH - 60, COL_DIM);

    drawCenteredText("Scan with any Lightning wallet",
                     SCREEN_WIDTH / 2, 625, 2, COL_FG, COL_BG);

    updateTimer(secsLeft, refreshCount);

    drawCenteredText("Tap to cancel", SCREEN_WIDTH / 2, SCREEN_HEIGHT - 30, 2, COL_DIM, COL_BG);
}

void DisplayUI::updateTimer(int secsLeft, int refreshCount) {
    _gfx->fillRect(0, 665, SCREEN_WIDTH, 80, COL_BG);

    char t[8]; snprintf(t, sizeof(t), ":%02d", secsLeft);
    drawCenteredText(t, SCREEN_WIDTH / 2, 690, 4,
                     secsLeft < 10 ? COL_ERROR : COL_FG, COL_BG);

    if (refreshCount > 0) {
        char r[32];
        snprintf(r, sizeof(r), "refresh %d/%d", refreshCount, MAX_INVOICE_REFRESHES);
        drawCenteredText(r, SCREEN_WIDTH / 2, 740, 2, COL_DIM, COL_BG);
    }
}

void DisplayUI::showPaid(uint64_t sats, float nzd) {
    _screen = Screen::PAID;
    _gfx->fillScreen(COL_BG);

    int cx = SCREEN_WIDTH / 2;

    int logoX = (SCREEN_WIDTH - LOGO_W) / 2;
    int logoY = 60;
    _gfx->draw16bitRGBBitmap(logoX, logoY, LOGO_RGB565, LOGO_W, LOGO_H);

    drawCenteredText("PAID", cx, 380, 7, COL_ACCENT, COL_BG);

    char info[64];
    if (nzd > 0) snprintf(info, sizeof(info), "$%.2f NZD", nzd);
    else         snprintf(info, sizeof(info), "%lu sats", (unsigned long)sats);
    drawCenteredText(info, cx, 490, 3, COL_FG, COL_BG);
    if (nzd > 0) {
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
