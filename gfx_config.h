#pragma once
// ============================================================
// MIPI-DSI display + GT911 touch init for
// Guition JC4880P443 (ESP32-P4, 4.3" 480x800 ST7701 MIPI-DSI panel).
// Touch handled by TAMC_GT911 library (battle-tested GT911 driver).
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include "config.h"

inline Arduino_GFX* createDSIDisplay() {
    // Working config from ESPHome PR #12068.
    auto* bus = new Arduino_ESP32DSIPanel(
        12  /* hsync_pulse_width */,
        42  /* hsync_back_porch  */,
        42  /* hsync_front_porch */,
        2   /* vsync_pulse_width */,
        8   /* vsync_back_porch  */,
        166 /* vsync_front_porch */,
        34000000 /* pixel clock */,
        500      /* lane bit rate Mbps */);

    return new Arduino_DSI_Display(
        SCREEN_WIDTH, SCREEN_HEIGHT, bus,
        SCREEN_ROTATION, true,
        LCD_RST_PIN,
        st7701_dsi_init_operations,
        sizeof(st7701_dsi_init_operations) / sizeof(st7701_dsi_init_operations[0]));
}

inline void panelPowerOn() {
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, HIGH);
}

// ---- Touch: TAMC_GT911 wrapper ----
inline TAMC_GT911& touch() {
    // Panel reports touches in native orientation (800 wide × 480 tall
    // landscape, per ESPHome). We rotate to portrait in gt911ReadTouch().
    static TAMC_GT911 ts(I2C_SDA_PIN, I2C_SCL_PIN,
                         TOUCH_INT_PIN, TOUCH_RST_PIN,
                         SCREEN_HEIGHT, SCREEN_WIDTH);
    return ts;
}

inline char* gt911StatusStr() {
    static char buf[48] = "GT911: not init";
    return buf;
}

inline void touchBegin() {
    touch().begin();
    touch().setRotation(ROTATION_NORMAL);
    strcpy(gt911StatusStr(), "GT911 ready");
}

// Compatibility shims so existing UI code that references these still links.
inline volatile uint8_t& gt911LastStatus() { static volatile uint8_t s = 0; return s; }
inline volatile int32_t& gt911LastRawX() { static volatile int32_t v = 0; return v; }
inline volatile int32_t& gt911LastRawY() { static volatile int32_t v = 0; return v; }

struct TouchCalib {
    int32_t rawTLx = 0,   rawTLy = 0;
    int32_t rawBRx = 800, rawBRy = 480;
    int32_t scrTLx = 0,   scrTLy = 0;
    int32_t scrBRx = SCREEN_WIDTH - 1, scrBRy = SCREEN_HEIGHT - 1;
    bool   done = false;
};
inline TouchCalib& touchCalib() { static TouchCalib c; return c; }

inline bool gt911ReadTouch(uint16_t* outX, uint16_t* outY) {
    auto& ts = touch();
    ts.read();
    if (!ts.isTouched || ts.touches == 0) return false;

    // Chip reports signed int16 but the library stores them as uint16.
    // Cast to int16_t so bottom-half Y values (which go negative) decode.
    // NO swap — chip's axes already align with portrait screen.
    int32_t rawX = (int16_t)ts.points[0].x;
    int32_t rawY = (int16_t)ts.points[0].y;

    gt911LastRawX() = rawX;
    gt911LastRawY() = rawY;
    gt911LastStatus() = 0x81;

    auto& c = touchCalib();
    if (!c.done) {
        *outX = rawX < 0 ? 0 : (rawX > 65535 ? 65535 : (uint16_t)rawX);
        *outY = rawY < 0 ? 0 : (rawY > 65535 ? 65535 : (uint16_t)rawY);
        return true;
    }

    int32_t dxRaw = c.rawBRx - c.rawTLx;
    int32_t dyRaw = c.rawBRy - c.rawTLy;
    int32_t dxScr = c.scrBRx - c.scrTLx;
    int32_t dyScr = c.scrBRy - c.scrTLy;
    if (dxRaw == 0) dxRaw = 1;
    if (dyRaw == 0) dyRaw = 1;

    int32_t sx = c.scrTLx + (rawX - c.rawTLx) * dxScr / dxRaw;
    int32_t sy = c.scrTLy + (rawY - c.rawTLy) * dyScr / dyRaw;

    if (sx < 0) sx = 0; if (sx >= SCREEN_WIDTH)  sx = SCREEN_WIDTH - 1;
    if (sy < 0) sy = 0; if (sy >= SCREEN_HEIGHT) sy = SCREEN_HEIGHT - 1;
    *outX = (uint16_t)sx;
    *outY = (uint16_t)sy;
    return true;
}
