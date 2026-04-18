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
inline volatile uint16_t& gt911LastRawX() { static volatile uint16_t v = 0; return v; }
inline volatile uint16_t& gt911LastRawY() { static volatile uint16_t v = 0; return v; }

inline bool gt911ReadTouch(uint16_t* outX, uint16_t* outY) {
    auto& ts = touch();
    ts.read();
    if (!ts.isTouched || ts.touches == 0) return false;

    // The library reports coords in the chip's native orientation we
    // configured (800 wide × 480 tall landscape). Rotate to portrait:
    //   portrait_x = raw_y
    //   portrait_y = max_x - raw_x  (mirror to keep origin top-left)
    uint16_t rawX = ts.points[0].x;
    uint16_t rawY = ts.points[0].y;
    gt911LastRawX() = rawX;
    gt911LastRawY() = rawY;
    gt911LastStatus() = 0x81;

    int32_t sx = rawY;
    int32_t sy = (int32_t)SCREEN_HEIGHT - 1 - rawX;
    if (sx < 0) sx = 0; if (sx >= SCREEN_WIDTH)  sx = SCREEN_WIDTH - 1;
    if (sy < 0) sy = 0; if (sy >= SCREEN_HEIGHT) sy = SCREEN_HEIGHT - 1;
    *outX = (uint16_t)sx;
    *outY = (uint16_t)sy;
    return true;
}
