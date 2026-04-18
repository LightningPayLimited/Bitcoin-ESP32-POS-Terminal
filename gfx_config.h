#pragma once
// ============================================================
// MIPI-DSI display + GT911 touch init for
// Guition JC4880P433 (ESP32-P4, 4.3" 480x800 JD9365 MIPI-DSI panel).
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "config.h"

inline Arduino_GFX* createDSIDisplay() {
    // Working config extracted from ESPHome PR #12068 (official JC4880P443
    // support). Panel driver is ST7701, NOT JD9365. Large vsync front
    // porch (166) is required for the low 34 MHz pixel clock to hit 60 Hz.
    auto* bus = new Arduino_ESP32DSIPanel(
        12  /* hsync_pulse_width */,
        42  /* hsync_back_porch  */,
        42  /* hsync_front_porch */,
        2   /* vsync_pulse_width */,
        8   /* vsync_back_porch  */,
        166 /* vsync_front_porch */,
        34000000 /* pixel clock (34 MHz) */,
        500      /* lane bit rate Mbps */);

    return new Arduino_DSI_Display(
        SCREEN_WIDTH, SCREEN_HEIGHT, bus,
        SCREEN_ROTATION, true /* auto flush */,
        LCD_RST_PIN,
        st7701_dsi_init_operations,
        sizeof(st7701_dsi_init_operations) / sizeof(st7701_dsi_init_operations[0]));
}

inline void panelPowerOn() {
    // Backlight on
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, HIGH);

    // LCD reset is driven by the Arduino_DSI_Display class itself
    // using LCD_RST_PIN, so we don't toggle it here.
}

// --- GT911 reset dance ---
// GT911 samples its INT pin state during reset to pick I2C address:
//   INT HIGH during reset -> 0x5D
//   INT LOW  during reset -> 0x14
// We want 0x5D (default) so hold INT high while releasing RST.
inline void gt911HardReset() {
    pinMode(TOUCH_RST_PIN, OUTPUT);
    pinMode(TOUCH_INT_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, LOW);
    digitalWrite(TOUCH_INT_PIN, HIGH);  // HIGH selects 0x5D
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(10);
    pinMode(TOUCH_INT_PIN, INPUT);      // release INT line as input
    delay(50);
}

// Set by touchBegin() so the UI can show it on screen.
inline char* gt911StatusStr() {
    static char buf[48] = "GT911: not init";
    return buf;
}
inline uint16_t& gt911MaxX() { static uint16_t v = 480; return v; }
inline uint16_t& gt911MaxY() { static uint16_t v = 800; return v; }

// Calibration state — set by DisplayUI::calibrateTouch()
struct TouchCalib {
    int16_t rawTlX = 0,   rawTlY = 0;    // raw at screen (TL_X, TL_Y)
    int16_t rawBrX = 480, rawBrY = 800;  // raw at screen (BR_X, BR_Y)
    int16_t tlX = 0,  tlY = 0;
    int16_t brX = 480, brY = 800;
    bool   done = false;
};
inline TouchCalib& touchCalib() { static TouchCalib c; return c; }

inline void applyTouchCalib(uint16_t rawX, uint16_t rawY,
                            uint16_t* outX, uint16_t* outY) {
    auto& c = touchCalib();
    if (!c.done) { *outX = rawX; *outY = rawY; return; }

    int32_t dx = c.rawBrX - c.rawTlX;
    int32_t dy = c.rawBrY - c.rawTlY;
    if (dx == 0) dx = 1;
    if (dy == 0) dy = 1;

    int32_t sx = c.tlX + ((int32_t)rawX - c.rawTlX) * (c.brX - c.tlX) / dx;
    int32_t sy = c.tlY + ((int32_t)rawY - c.rawTlY) * (c.brY - c.tlY) / dy;

    if (sx < 0) sx = 0; if (sx >= SCREEN_WIDTH)  sx = SCREEN_WIDTH  - 1;
    if (sy < 0) sy = 0; if (sy >= SCREEN_HEIGHT) sy = SCREEN_HEIGHT - 1;
    *outX = (uint16_t)sx;
    *outY = (uint16_t)sy;
}

inline void touchBegin() {
    gt911HardReset();
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    delay(10);

    // Read product ID (4 bytes at 0x8140) — should be "911\0"
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(0x81);
    Wire.write(0x40);
    if (Wire.endTransmission() != 0) {
        strcpy(gt911StatusStr(), "GT911: I2C nack");
        return;
    }
    Wire.requestFrom(GT911_ADDR, (uint8_t)4);
    char id[5] = {0};
    for (int i = 0; i < 4 && Wire.available(); i++) id[i] = Wire.read();

    // Read configured output resolution (0x8146..0x8149, little-endian words)
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(0x81);
    Wire.write(0x46);
    Wire.endTransmission(false);
    Wire.requestFrom(GT911_ADDR, (uint8_t)4);
    uint8_t xl = Wire.read(), xh = Wire.read(), yl = Wire.read(), yh = Wire.read();
    gt911MaxX() = xl | (xh << 8);
    gt911MaxY() = yl | (yh << 8);

    snprintf(gt911StatusStr(), 48, "GT911 '%s' %ux%u",
             id, gt911MaxX(), gt911MaxY());
}

inline void gt911WriteReg(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(val);
    Wire.endTransmission();
}

// Exposed so UI can show the latest status byte + coords for diagnosis.
inline volatile uint8_t& gt911LastStatus() {
    static volatile uint8_t s = 0;
    return s;
}
inline volatile uint16_t& gt911LastRawX() { static volatile uint16_t v = 0; return v; }
inline volatile uint16_t& gt911LastRawY() { static volatile uint16_t v = 0; return v; }

inline bool gt911ReadTouch(uint16_t* outX, uint16_t* outY) {
    // Atomic 8-byte read from 0x814E gets status + key + full point-1 data.
    // Layout:
    //   [0] 0x814E  status    (bit7 ready, bit3..0 point count)
    //   [1] 0x814F  keys
    //   [2] 0x8150  point1 track_id
    //   [3] 0x8151  point1 x_low
    //   [4] 0x8152  point1 x_high
    //   [5] 0x8153  point1 y_low
    //   [6] 0x8154  point1 y_high
    //   [7] 0x8155  point1 size_low
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(0x81);
    Wire.write(0x4E);
    if (Wire.endTransmission(false) != 0) return false;
    int got = Wire.requestFrom(GT911_ADDR, (uint8_t)8);
    if (got != 8) return false;

    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = Wire.read();

    uint8_t status = buf[0];
    gt911LastStatus() = status;

    bool ready = (status & 0x80);
    uint8_t points = status & 0x0F;
    bool have = ready && (points > 0);

    if (have) {
        // Standard GT911: X and Y are little-endian 16-bit words
        uint16_t rawX = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
        uint16_t rawY = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
        gt911LastRawX() = rawX;
        gt911LastRawY() = rawY;

        applyTouchCalib(rawX, rawY, outX, outY);
    }

    if (ready) gt911WriteReg(0x814E, 0x00);
    return have;
}
