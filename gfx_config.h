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

    // Pass the NATIVE panel resolution (PANEL_W x PANEL_H); rotation 1/3 makes
    // the logical canvas landscape (gfx->width()==SCREEN_WIDTH==800).
    return new Arduino_DSI_Display(
        PANEL_W, PANEL_H, bus,
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
    // Construct with the panel's native touch frame (800 wide × 480 tall).
    // We read raw points and map them ourselves in gt911ReadTouch().
    static TAMC_GT911 ts(I2C_SDA_PIN, I2C_SCL_PIN,
                         TOUCH_INT_PIN, TOUCH_RST_PIN,
                         PANEL_H, PANEL_W);
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

// Landscape calibration. After the optional axis swap (TOUCH_SWAP_XY), the
// "X source" raw value maps to screen X via [xRaw0..xRaw1] -> [0..W-1], and
// likewise the "Y source" via [yRaw0..yRaw1] -> [0..H-1]. Endpoints come from
// the panel's observed raw extremes; TOUCH_INVERT_X/_Y flip a screen axis if
// the device's physical orientation runs the other way.
struct TouchCalib {
    int32_t xRaw0 = 490,  xRaw1 = -310;   // X source raw at screen left .. right
    int32_t yRaw0 = 800,  yRaw1 = 340;    // Y source raw at screen top  .. bottom
    bool   done = true;
};
inline TouchCalib& touchCalib() { static TouchCalib c; return c; }

inline bool gt911ReadTouch(uint16_t* outX, uint16_t* outY) {
    auto& ts = touch();
    ts.read();
    if (!ts.isTouched || ts.touches == 0) return false;

    // Chip reports signed int16 but the library stores them as uint16.
    // Cast to int16_t so values that go negative decode correctly.
    int32_t rawX = (int16_t)ts.points[0].x;
    int32_t rawY = (int16_t)ts.points[0].y;

    gt911LastRawX() = rawX;
    gt911LastRawY() = rawY;
    gt911LastStatus() = 0x81;

#if TOUCH_SWAP_XY
    int32_t xSrc = rawY, ySrc = rawX;   // landscape: raw Y drives screen X
#else
    int32_t xSrc = rawX, ySrc = rawY;
#endif

    auto& c = touchCalib();
    int32_t dxRaw = c.xRaw1 - c.xRaw0; if (dxRaw == 0) dxRaw = 1;
    int32_t dyRaw = c.yRaw1 - c.yRaw0; if (dyRaw == 0) dyRaw = 1;

    int32_t sx = (xSrc - c.xRaw0) * (SCREEN_WIDTH  - 1) / dxRaw;
    int32_t sy = (ySrc - c.yRaw0) * (SCREEN_HEIGHT - 1) / dyRaw;

#if TOUCH_INVERT_X
    sx = (SCREEN_WIDTH  - 1) - sx;
#endif
#if TOUCH_INVERT_Y
    sy = (SCREEN_HEIGHT - 1) - sy;
#endif

    if (sx < 0) sx = 0; if (sx >= SCREEN_WIDTH)  sx = SCREEN_WIDTH - 1;
    if (sy < 0) sy = 0; if (sy >= SCREEN_HEIGHT) sy = SCREEN_HEIGHT - 1;
    *outX = (uint16_t)sx;
    *outY = (uint16_t)sy;
    return true;
}
