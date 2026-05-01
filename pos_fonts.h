#pragma once
// Sans-serif (Helvetica-like) fonts for the POS UI.
// Drop-in replacement for the default 5x7 Adafruit GFX font.
#include "fonts/FreeSans12pt7b.h"
#include "fonts/FreeSans18pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"
#include <Arduino_GFX_Library.h>

// Map the legacy integer "size" used throughout the UI (1..10) onto a
// concrete GFXfont + integer scale factor. Picked so that the rendered text
// height roughly matches the previous default-font sizes:
//   size 1 (~12 px) → FreeSans 12pt
//   size 2 (~14 px) → FreeSans 12pt
//   size 3 (~21 px) → FreeSansBold 18pt
//   size 4 (~28 px) → FreeSansBold 24pt
//   size 5 (~35 px) → FreeSansBold 24pt
//   size 7 (~49 px) → FreeSansBold 24pt @ scale 2
//   size 10 (~70 px) → FreeSansBold 24pt @ scale 3
inline void applyPosFont(Arduino_GFX* gfx, int size) {
    const GFXfont* f;
    int scale = 1;
    switch (size) {
        case 1: f = &FreeSans12pt7b;     break;
        case 2: f = &FreeSans12pt7b;     break;
        case 3: f = &FreeSansBold18pt7b; break;
        case 4: f = &FreeSansBold24pt7b; break;
        case 5: f = &FreeSansBold24pt7b; break;
        case 7: f = &FreeSansBold24pt7b; scale = 2; break;
        default:
        case 10: f = &FreeSansBold24pt7b; scale = 3; break;
    }
    gfx->setFont(f);
    gfx->setTextSize(scale);
}
