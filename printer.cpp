#include "printer.h"
#include "config.h"
#include "logo.h"
#include <Adafruit_Thermal.h>
#include <HardwareSerial.h>

// Use UART1 — UART0 is the USB/serial console.
static HardwareSerial    printerSerial(1);
static Adafruit_Thermal  thermal(&printerSerial);

Printer printer;

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
    // Hard reset to clear any residual state the library's setDefault() might
    // not touch on a clone (ESC @ — full re-initialise).
    printerSerial.write(0x1B);
    printerSerial.write(0x40);
    delay(50);
    thermal.setDefault();
    thermal.setLineHeight(20);
    // Thermal printer character cells are 12 wide × 24 tall by default — a
    // 1:2 aspect that reads as "stretched". Set ESC ! with bit 5 (double
    // width) so each cell becomes 24×24 — a clean square per character.
    //   bit 0 = Font B, bit 3 = emphasised, bit 4 = double-height,
    //   bit 5 = double-width, bit 7 = underline
    printerSerial.write(0x1B);
    printerSerial.write(0x21);
    printerSerial.write(0x20);

    // ---- Logo at top, centred ----
    // The bitmap is pre-padded to full paper width (LOGO_PRINT_W = 384) with
    // the actual logo offset to land centred — DC2 * always prints from the
    // left margin and ignores justify('C').
    thermal.justify('C');
    thermal.printBitmap(LOGO_PRINT_W, LOGO_PRINT_H, LOGO_MONO);
    thermal.feed(1);

    // ---- Header: merchant name (bold for emphasis) ----
    thermal.boldOn();
    thermal.println(merchantName.length() ? merchantName : "STACKED POS");
    thermal.boldOff();
    if (gstNumber.length()) {
        thermal.printf("GST: %s\n", gstNumber.c_str());
    }
    thermal.println();

    // ---- PAID banner ----
    thermal.boldOn();
    thermal.println("PAID");
    thermal.boldOff();
    thermal.println("Bitcoin / Lightning");
    thermal.println();

    // ---- Amounts (left-aligned) ----
    thermal.justify('L');
    thermal.boldOn();
    char nzdBuf[32];
    snprintf(nzdBuf, sizeof(nzdBuf), "$%.2f NZD", nzdAmount);
    thermal.println(nzdBuf);
    thermal.boldOff();
    thermal.printf("%lu sats\n", (unsigned long)satAmount);
    thermal.println();

    // ---- Metadata ----
    if (paidDate.length()) thermal.printf("Paid:  %s\n", paidDate.c_str());
    if (reference.length()) thermal.printf("Ref:   %s\n", reference.c_str());

    thermal.println();
    thermal.justify('C');
    thermal.println("Thank you!");
    thermal.println();
    thermal.feed(3);
    thermal.sleep();
}
