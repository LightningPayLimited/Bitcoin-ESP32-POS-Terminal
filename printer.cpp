#include "printer.h"
#include "config.h"
#include "logo.h"
#include "pos_fonts.h"
#include <Adafruit_Thermal.h>
#include <Arduino_GFX_Library.h>
#include <HardwareSerial.h>

// Use UART1 — UART0 is the USB/serial console.
static HardwareSerial    printerSerial(1);
static Adafruit_Thermal  thermal(&printerSerial);

Printer printer;

// Send a 1-bpp bitmap using GS v 0 (raster format). This clone ignores the
// older DC2 * command and — as we discovered the hard way — also ignores
// every standard ESC/GS font, size, and line-spacing command for character
// mode. So ALL receipt content (logo and text) goes through this path.
static void printBitmapRaster(uint16_t w, uint16_t h, const uint8_t* data) {
    const uint16_t widthBytes = (w + 7) / 8;
    // GS v 0 m xL xH yL yH
    printerSerial.write(0x1D);
    printerSerial.write(0x76);
    printerSerial.write(0x30);
    printerSerial.write((uint8_t)0);                          // m = normal
    printerSerial.write((uint8_t)(widthBytes & 0xFF));
    printerSerial.write((uint8_t)((widthBytes >> 8) & 0xFF));
    printerSerial.write((uint8_t)(h & 0xFF));
    printerSerial.write((uint8_t)((h >> 8) & 0xFF));
    const uint32_t total = (uint32_t)widthBytes * h;
    for (uint32_t i = 0; i < total; i++) {
        // pgm_read_byte is safe for both PROGMEM (logo) and RAM (canvas) on ESP32.
        printerSerial.write(pgm_read_byte(&data[i]));
    }
}

// Advance paper by N dots — independent of the printer's line-spacing
// register, which this clone won't honour.
static void feedDots(uint8_t dots) {
    printerSerial.write(0x1B);
    printerSerial.write(0x4A);
    printerSerial.write(dots);
}

// Rasterise one line of text into a paper-wide 1-bpp bitmap and print it.
// canvasH is the bitmap height in dots; pick a value that comfortably fits
// the chosen font (yAdvance + a couple of dots of headroom).
static void printText(const String& s,
                      const GFXfont* font,
                      char justify = 'L',
                      int16_t canvasH = 32) {
    if (!s.length()) return;
    const int16_t W = LOGO_PRINT_W;  // 384 dots = full paper width

    Arduino_Canvas_Mono canvas(W, canvasH, nullptr);
    if (!canvas.begin()) return;
    // begin() malloc'd the buffer but left it uninitialised — wipe to white
    // paper (every bit = 0). Much faster than fillScreen() which iterates
    // pixel-by-pixel.
    const size_t bufSize = (size_t)((W + 7) / 8) * (size_t)canvasH;
    memset(canvas.getFramebuffer(), 0, bufSize);

    canvas.setFont(font);
    canvas.setTextColor(RGB565_WHITE);  // canvas: WHITE sets bit = black ink
    canvas.setTextSize(1);

    int16_t x1, y1; uint16_t tw, th;
    canvas.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);

    int16_t drawX;
    switch (justify) {
        case 'C': drawX = (W - (int16_t)tw) / 2 - x1; break;
        case 'R': drawX =  W - (int16_t)tw     - x1; break;
        default:  drawX = -x1;                        break;  // 'L'
    }
    // Baseline near the bottom of the canvas with a small descender gap.
    const int16_t drawY = canvasH - 4;

    canvas.setCursor(drawX, drawY);
    canvas.print(s);

    printBitmapRaster(W, canvasH, canvas.getFramebuffer());
    feedDots(6);  // small inter-line gap so adjacent rows don't touch
}

void Printer::begin() {
    printerSerial.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
    delay(100);
    thermal.begin();
    // No reliable way to detect "is a printer attached" over plain TTL,
    // so we always treat begin() as success. If nothing is connected the
    // bytes simply float away into the ether.
    _ready = true;
    Serial.printf("[PRINTER] begin on TX=%d RX=%d @ %d baud\n",
                  PRINTER_TX_PIN, PRINTER_RX_PIN, PRINTER_BAUD);
}

void Printer::printReceipt(const String& merchantName,
                           const String& gstNumber,
                           float         nzdAmount,
                           uint64_t      satAmount,
                           const String& reference,
                           const String& paidDate) {
    if (!_ready) return;

    thermal.wake();
    // ESC @ — full re-initialise.
    printerSerial.write(0x1B);
    printerSerial.write(0x40);
    delay(50);

    // ---- Logo at top, centred ----
    printBitmapRaster(LOGO_PRINT_W, LOGO_PRINT_H, LOGO_MONO);
    feedDots(8);

    // ---- Header: merchant name + optional GST ----
    printText(merchantName.length() ? merchantName : "STACKED POS",
              &FreeSansBold18pt7b, 'C', 40);
    if (gstNumber.length()) {
        printText("GST: " + gstNumber, &FreeSans12pt7b, 'C', 28);
    }
    feedDots(12);

    // ---- PAID banner ----
    printText("PAID", &FreeSansBold24pt7b, 'C', 52);
    printText("Bitcoin / Lightning", &FreeSans12pt7b, 'C', 28);
    feedDots(12);

    // ---- Amounts (left-aligned) ----
    char buf[64];
    snprintf(buf, sizeof(buf), "$%.2f NZD", nzdAmount);
    printText(buf, &FreeSansBold18pt7b, 'L', 40);
    snprintf(buf, sizeof(buf), "%lu sats", (unsigned long)satAmount);
    printText(buf, &FreeSans12pt7b, 'L', 28);
    feedDots(12);

    // ---- Metadata ----
    if (paidDate.length()) {
        printText("Paid:  " + paidDate, &FreeSans12pt7b, 'L', 28);
    }
    if (reference.length()) {
        printText("Ref:   " + reference, &FreeSans12pt7b, 'L', 28);
    }
    feedDots(16);

    printText("Thank you!", &FreeSans12pt7b, 'C', 28);
    feedDots(160);  // tear-off gap

    thermal.sleep();
}
