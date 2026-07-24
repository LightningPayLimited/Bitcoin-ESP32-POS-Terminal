#include "display_ui.h"
#include "config.h"
#include <Wire.h>
#include <qrcode.h>

// QR screen layout (240x320): big QR up top, amount + countdown below.
#define QR_AREA   234
#define QR_X      3
#define QR_Y      8
#define CD_Y      288   // countdown text baseline region

// Close button on the QR/invoice screen (bottom-right)
#define QRC_X     168
#define QRC_Y     252
#define QRC_W     66
#define QRC_H     60

// ---- Numpad geometry (240x320 portrait) ----
#define AMT_H    66          // amount bar height
#define KP_X0    6
#define KP_Y0    74
#define KP_W     74
#define KP_H     46
#define KP_GAP   6
#define PAY_Y    282
#define PAY_H    32

// 3 cols x 4 rows
static const Key KEYMAP[4][3] = {
    { Key::D1,  Key::D2, Key::D3  },
    { Key::D4,  Key::D5, Key::D6  },
    { Key::D7,  Key::D8, Key::D9  },
    { Key::DOT, Key::D0, Key::DEL },
};
static const char* LABELS[4][3] = {
    { "1", "2", "3" },
    { "4", "5", "6" },
    { "7", "8", "9" },
    { ".", "0", "DEL" },
};

// ---- Display + touch objects ----
static Arduino_DataBus* s_bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
static Arduino_GFX* s_gfx =
    new Arduino_ILI9341(s_bus, LCD_RST, 0 /*rotation*/, false /*IPS*/);

void DisplayUI::begin() {
    _gfx = s_gfx;
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    _gfx->begin();
    _gfx->fillScreen(COL_BG);

    // FT6336 touch
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);  delay(10);
    digitalWrite(TOUCH_RST, HIGH); delay(60);
}

// ---- Text helpers (built-in 5x7 font; 6px advance, 8px line * size) ----
void DisplayUI::centerText(const String& s, int cx, int cy, int size, uint16_t fg) {
    int w = s.length() * 6 * size;
    int h = 8 * size;
    _gfx->setTextColor(fg);
    _gfx->setTextSize(size);
    _gfx->setCursor(cx - w / 2, cy - h / 2);
    _gfx->print(s);
}

void DisplayUI::drawButton(int x, int y, int w, int h, const String& label,
                           uint16_t bg, uint16_t fg, int textSize) {
    _gfx->fillRoundRect(x, y, w, h, 6, bg);
    centerText(label, x + w / 2, y + h / 2, textSize, fg);
}

// ============================================================
// Screens
// ============================================================
void DisplayUI::showSplash(const String& merchantName) {
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 4, COL_ACCENT);
    _gfx->fillRect(0, SCREEN_HEIGHT - 4, SCREEN_WIDTH, 4, COL_ACCENT);

    centerText("LIGHTNING", SCREEN_WIDTH / 2, 120, 3, COL_ACCENT);
    centerText("PAY",       SCREEN_WIDTH / 2, 152, 3, COL_ACCENT);
    centerText("Bitcoin Point of Sale", SCREEN_WIDTH / 2, 200, 1, COL_FG);
    if (merchantName.length())
        centerText(merchantName, SCREEN_WIDTH / 2, 230, 2, COL_DIM);
}

void DisplayUI::showSetupInfo(const String& ssid, const String& ip) {
    _gfx->fillScreen(COL_BG);
    _gfx->fillRect(0, 0, SCREEN_WIDTH, 4, COL_ACCENT);
    _gfx->fillRect(0, SCREEN_HEIGHT - 4, SCREEN_WIDTH, 4, COL_ACCENT);

    centerText("LIGHTNING PAY", SCREEN_WIDTH / 2, 24, 2, COL_ACCENT);
    centerText("POS SETUP",     SCREEN_WIDTH / 2, 50, 1, COL_DIM);

    centerText("1. Join WiFi network:", SCREEN_WIDTH / 2, 110, 1, COL_FG);
    centerText(ssid,                    SCREEN_WIDTH / 2, 134, 2, COL_ACCENT);

    centerText("2. Open browser to:", SCREEN_WIDTH / 2, 184, 1, COL_FG);
    centerText(ip,                     SCREEN_WIDTH / 2, 208, 2, COL_ACCENT);

    centerText("Enter WiFi + API key", SCREEN_WIDTH / 2, 268, 1, COL_DIM);
}

// Repaint just the amount bar — called on every keypress so the keypad
// itself doesn't get redrawn (full-screen redraws flash visibly).
void DisplayUI::updateAmount(const String& amount, const String& currency) {
    _gfx->fillRect(0, 0, SCREEN_WIDTH, AMT_H, COL_KEYPAD_BG);
    String amt = amount.length() ? amount : "0";
    centerText("$" + amt, SCREEN_WIDTH / 2, 26, 3, COL_FG);
    centerText(currency,  SCREEN_WIDTH / 2, 52, 1, COL_DIM);
}

void DisplayUI::showAmountEntry(const String& amount, const String& currency) {
    _gfx->fillScreen(COL_BG);

    updateAmount(amount, currency);

    // Numpad
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int x = KP_X0 + c * (KP_W + KP_GAP);
            int y = KP_Y0 + r * (KP_H + KP_GAP);
            drawButton(x, y, KP_W, KP_H, LABELS[r][c],
                       COL_KEYPAD_BG, COL_KEYPAD_FG, 3);
        }
    }

    // PAY
    drawButton(KP_X0, PAY_Y, SCREEN_WIDTH - 2 * KP_X0, PAY_H, "PAY",
               COL_ACCENT, COL_BG, 2);
}

void DisplayUI::showMessage(const String& title, const String& body, uint16_t accent) {
    _gfx->fillScreen(COL_BG);
    centerText(title, SCREEN_WIDTH / 2, 120, 3, accent);
    centerText(body,  SCREEN_WIDTH / 2, 170, 2, COL_FG);
}

void DisplayUI::showLoading(const String& message) {
    _gfx->fillScreen(COL_BG);
    centerText(message, SCREEN_WIDTH / 2, 150, 2, COL_FG);
    centerText(". . .", SCREEN_WIDTH / 2, 185, 2, COL_ACCENT);
}

void DisplayUI::drawQRCode(const String& data, int x, int y, int areaSize) {
    QRCode qr;
    uint8_t buf[qrcode_getBufferSize(QR_VERSION)];
    qrcode_initText(&qr, buf, QR_VERSION, QR_ECC_LEVEL, data.c_str());

    int modPx = areaSize / qr.size;
    if (modPx < 1) modPx = 1;
    int totalPx = modPx * qr.size;
    int ox = x + (areaSize - totalPx) / 2;
    int oy = y + (areaSize - totalPx) / 2;

    // White quiet-zone background, then black modules.
    _gfx->fillRect(x - 3, y - 3, areaSize + 6, areaSize + 6, RGB565_WHITE);
    for (uint8_t qy = 0; qy < qr.size; qy++)
        for (uint8_t qx = 0; qx < qr.size; qx++)
            if (qrcode_getModule(&qr, qx, qy))
                _gfx->fillRect(ox + qx * modPx, oy + qy * modPx, modPx, modPx, RGB565_BLACK);
}

void DisplayUI::showQR(const String& bolt11, uint64_t sats, float fiat,
                       const String& currency, int secsLeft) {
    _gfx->fillScreen(COL_BG);
    drawQRCode(bolt11, QR_X, QR_Y, QR_AREA);

    // Bottom strip: amounts + countdown on the left, close button on the right.
    char line[48];
    snprintf(line, sizeof(line), "$%.2f %s", fiat, currency.c_str());
    centerText(line, QRC_X / 2, 262, 2, COL_FG);
    snprintf(line, sizeof(line), "%lu sats", (unsigned long)sats);
    centerText(line, QRC_X / 2, 284, 1, COL_DIM);

    drawButton(QRC_X, QRC_Y, QRC_W, QRC_H, "X", COL_KEYPAD_BG, COL_FG, 3);

    updateCountdown(secsLeft);
}

void DisplayUI::updateCountdown(int secsLeft) {
    _gfx->fillRect(0, CD_Y + 12, QRC_X, 18, COL_BG);   // clear old text (left of X)
    if (secsLeft < 0) secsLeft = 0;
    char line[24];
    snprintf(line, sizeof(line), "Expires in %ds", secsLeft);
    centerText(line, QRC_X / 2, CD_Y + 20, 1, COL_ACCENT);
}

void DisplayUI::showPaid(uint64_t sats, float fiat, const String& currency) {
    _gfx->fillScreen(COL_BG);
    centerText("PAID", SCREEN_WIDTH / 2, 110, 4, COL_SUCCESS);
    char line[48];
    snprintf(line, sizeof(line), "$%.2f %s", fiat, currency.c_str());
    centerText(line, SCREEN_WIDTH / 2, 165, 2, COL_FG);
    snprintf(line, sizeof(line), "%lu sats", (unsigned long)sats);
    centerText(line, SCREEN_WIDTH / 2, 190, 1, COL_DIM);
    centerText("Tap to continue", SCREEN_WIDTH / 2, 280, 1, COL_DIM);
}

void DisplayUI::showError(const String& message) {
    _gfx->fillScreen(COL_BG);
    centerText("Error", SCREEN_WIDTH / 2, 120, 3, COL_ERROR);
    centerText(message, SCREEN_WIDTH / 2, 165, 1, COL_FG);
    centerText("Tap to continue", SCREEN_WIDTH / 2, 280, 1, COL_DIM);
}

// ============================================================
// Touch (FT6336)
// ============================================================
bool DisplayUI::readTouchPoint(int& x, int& y) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(TOUCH_ADDR, 5) != 5) return false;

    uint8_t points = Wire.read() & 0x0F;
    uint8_t xh = Wire.read(), xl = Wire.read();
    uint8_t yh = Wire.read(), yl = Wire.read();
    if (points == 0 || points > 5) return false;

    x = ((xh & 0x0F) << 8) | xl;
    y = ((yh & 0x0F) << 8) | yl;
    return true;
}

Key DisplayUI::hitTest(int x, int y) {
    if (y < AMT_H) return Key::CLEAR;                 // tap amount bar = clear
    if (y >= PAY_Y) return Key::CHARGE;               // PAY button

    int c = (x - KP_X0) / (KP_W + KP_GAP);
    int r = (y - KP_Y0) / (KP_H + KP_GAP);
    if (r < 0 || r > 3 || c < 0 || c > 2) return Key::NONE;
    // reject taps in the gaps
    int bx = KP_X0 + c * (KP_W + KP_GAP);
    int by = KP_Y0 + r * (KP_H + KP_GAP);
    if (x < bx || x > bx + KP_W || y < by || y > by + KP_H) return Key::NONE;
    return KEYMAP[r][c];
}

Key DisplayUI::pollTouch() {
    int x, y;
    bool touched = readTouchPoint(x, y);
    if (touched && !_touchDown && (millis() - _lastTouchMs > 200)) {
        _touchDown = true;
        _lastTouchMs = millis();
        return hitTest(x, y);
    }
    if (!touched) _touchDown = false;
    return Key::NONE;
}

bool DisplayUI::qrCloseTouched() {
    int x, y;
    bool touched = readTouchPoint(x, y);
    if (touched && !_touchDown && (millis() - _lastTouchMs > 250)) {
        _touchDown = true;
        _lastTouchMs = millis();
        return x >= QRC_X && x <= QRC_X + QRC_W &&
               y >= QRC_Y && y <= QRC_Y + QRC_H;
    }
    if (!touched) _touchDown = false;
    return false;
}

bool DisplayUI::anyTouch() {
    int x, y;
    bool touched = readTouchPoint(x, y);
    if (touched && !_touchDown && (millis() - _lastTouchMs > 250)) {
        _touchDown = true;
        _lastTouchMs = millis();
        return true;
    }
    if (!touched) _touchDown = false;
    return false;
}
