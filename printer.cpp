#include "printer.h"
#include "config.h"
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
    thermal.setDefault();

    // ---- Header: merchant name centred + bold ----
    thermal.justify('C');
    thermal.boldOn();
    thermal.setSize('L');
    thermal.println(merchantName.length() ? merchantName : "STACKED POS");
    thermal.setSize('S');
    thermal.boldOff();
    if (gstNumber.length()) {
        thermal.printf("GST: %s\n", gstNumber.c_str());
    }
    thermal.println();

    // ---- Big PAID banner ----
    thermal.boldOn();
    thermal.setSize('L');
    thermal.println("PAID");
    thermal.boldOff();
    thermal.setSize('M');
    thermal.println("Bitcoin / Lightning");
    thermal.println();

    // ---- Amounts (left-aligned) ----
    thermal.justify('L');
    thermal.setSize('L');
    char nzdBuf[32];
    snprintf(nzdBuf, sizeof(nzdBuf), "$%.2f NZD", nzdAmount);
    thermal.println(nzdBuf);
    thermal.setSize('S');
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
