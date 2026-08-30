#include "printer.h"
#include "config.h"
#include "logo.h"
#include "pos_fonts.h"
#include "money_fmt.h"
#include <Adafruit_Thermal.h>
#include <Arduino_GFX_Library.h>
#include <HardwareSerial.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Use UART1 — UART0 is the USB/serial console.
static HardwareSerial    printerSerial(1);
static Adafruit_Thermal  thermal(&printerSerial);

Printer printer;

// A queued receipt. Printing happens on a background task so the slow
// (9600-baud) UART writes don't stall the UI loop. Jobs are heap-allocated
// and the pointer is passed through the queue — copying a struct full of
// Strings byte-for-byte through a FreeRTOS queue would double-free.
struct ReceiptJob {
    String   merchantName;
    String   gstNumber;
    String   currency;
    String   reference;
    String   paidDate;
    String   payee;
    float    fiat;
    uint64_t sats;
};

static QueueHandle_t printQueue = nullptr;
static void printerTask(void* arg);

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
    canvas.setTextWrap(false);          // clip long lines; never wrap off-canvas

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

// Unwrapped pixel width of `s` in `font` (small probe canvas, freed on return).
static uint16_t textWidth(const String& s, const GFXfont* font) {
    Arduino_Canvas_Mono probe(LOGO_PRINT_W, 8, nullptr);
    if (!probe.begin()) return 0;
    probe.setFont(font);
    probe.setTextSize(1);
    probe.setTextWrap(false);
    int16_t x1, y1; uint16_t w, h;
    probe.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return w;
}

void Printer::begin() {
    printerSerial.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
    delay(100);
    thermal.begin();
    // No reliable way to detect "is a printer attached" over plain TTL,
    // so we always treat begin() as success. If nothing is connected the
    // bytes simply float away into the ether.
    _ready = true;

    // Background print task — receipts render here so the UI loop never
    // blocks on the slow UART. Queue holds heap ReceiptJob pointers.
    printQueue = xQueueCreate(4, sizeof(ReceiptJob*));
    if (printQueue) {
        xTaskCreatePinnedToCore(printerTask, "receipt", 8192, nullptr,
                                1 /* low priority */, nullptr, tskNO_AFFINITY);
    } else {
        Serial.println("[PRINTER] queue alloc failed — printing disabled");
        _ready = false;
    }

    Serial.printf("[PRINTER] begin on TX=%d RX=%d @ %d baud\n",
                  PRINTER_TX_PIN, PRINTER_RX_PIN, PRINTER_BAUD);
}

// Render + emit one receipt. Runs ONLY on the background print task.
static void renderReceipt(const ReceiptJob& j) {
    thermal.wake();
    // ESC @ — full re-initialise.
    printerSerial.write(0x1B);
    printerSerial.write(0x40);
    delay(50);

    // ---- Logo at top, centred ----
    printBitmapRaster(LOGO_PRINT_W, LOGO_PRINT_H, LOGO_MONO);
    feedDots(8);

    // ---- Header: merchant name + optional GST ----
    // Self-custody with no store name reports the address as the merchant
    // name; it's on the "Paid to" line already and too wide for 18 pt.
    const bool nameIsPayee = j.payee.length() && j.merchantName == j.payee;
    printText((j.merchantName.length() && !nameIsPayee) ? j.merchantName
                                                        : "LIGHTNING PAY",
              &FreeSansBold18pt7b, 'C', 40);
    if (j.gstNumber.length()) {
        printText("GST: " + j.gstNumber, &FreeSans12pt7b, 'C', 28);
    }
    feedDots(12);

    // ---- PAID banner ----
    printText("PAID", &FreeSansBold24pt7b, 'C', 52);
    printText("Bitcoin / Lightning", &FreeSans12pt7b, 'C', 28);
    feedDots(12);

    // ---- Amounts (left-aligned) ----
    String cur = j.currency.length() ? j.currency : String("NZD");
    char buf[64];
    printText(formatAmount(j.fiat, j.sats, cur), &FreeSansBold18pt7b, 'L', 40);
    if (cur != "SATS") {   // sats-denominated sale: no second line
        snprintf(buf, sizeof(buf), "%lu sats", (unsigned long)j.sats);
        printText(buf, &FreeSans12pt7b, 'L', 28);
    }
    feedDots(12);

    // ---- Metadata ----
    if (j.paidDate.length()) {
        printText("Paid:  " + j.paidDate, &FreeSans12pt7b, 'L', 28);
    }
    if (j.reference.length()) {
        printText("Ref:   " + j.reference, &FreeSans12pt7b, 'L', 28);
    }
    if (j.payee.length()) {
        // 384 dots fit ~30 chars at 12 pt; longer addresses go on their
        // own line(s): split at '@', then anything still too wide is
        // broken at a '.', '-' or '/' (or by character) — never clipped.
        const GFXfont* f = &FreeSans12pt7b;
        auto emit = [&](String txt) {
            while (txt.length()) {
                String seg = txt;
                while (textWidth(seg, f) > LOGO_PRINT_W && seg.length() > 1) {
                    int cut = seg.lastIndexOf('.');
                    cut = max(cut, seg.lastIndexOf('-'));
                    cut = max(cut, seg.lastIndexOf('/'));
                    seg = (cut > 0) ? seg.substring(0, cut + 1)
                                    : seg.substring(0, seg.length() - 1);
                }
                printText(seg, f, 'L', 28);
                txt = txt.substring(seg.length());
            }
        };
        String line = "Paid to: " + j.payee;
        int at = j.payee.indexOf('@');
        if (textWidth(line, f) <= LOGO_PRINT_W) {
            printText(line, f, 'L', 28);
        } else {
            printText("Paid to:", f, 'L', 28);
            if (at > 0 && textWidth(j.payee, f) > LOGO_PRINT_W) {
                emit(j.payee.substring(0, at + 1));   // user@
                emit(j.payee.substring(at + 1));      // domain
            } else {
                emit(j.payee);
            }
        }
    }
    feedDots(16);

    printText("Thank you!", &FreeSans12pt7b, 'C', 28);
    feedDots(160);  // tear-off gap

    thermal.sleep();
}

// Background task: drain the queue, print each receipt, free the job.
static void printerTask(void* /*arg*/) {
    for (;;) {
        ReceiptJob* job = nullptr;
        if (xQueueReceive(printQueue, &job, portMAX_DELAY) == pdTRUE && job) {
            unsigned long t0 = millis();
            renderReceipt(*job);
            Serial.printf("[PRINTER] receipt done in %lums\n", millis() - t0);
            delete job;
        }
    }
}

void Printer::printReceipt(const String& merchantName,
                           const String& gstNumber,
                           const String& currency,
                           float         fiatAmount,
                           uint64_t      satAmount,
                           const String& reference,
                           const String& paidDate,
                           const String& payee) {
    if (!_ready || !printQueue) return;

    ReceiptJob* job = new ReceiptJob{merchantName, gstNumber, currency,
                                     reference, paidDate, payee,
                                     fiatAmount, satAmount};
    // Non-blocking enqueue — never stall the caller (the UI loop).
    if (xQueueSend(printQueue, &job, 0) != pdTRUE) {
        Serial.println("[PRINTER] queue full — dropping receipt");
        delete job;
    }
}
