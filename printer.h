#pragma once
#include <Arduino.h>

// ============================================================
// Thermal printer (Adafruit CSN-A2 / generic ESC-POS over TTL).
// Prints a Bitcoin payment receipt after a successful sale.
// Safe to call printReceipt() even if the printer isn't connected —
// init() will fail silently and subsequent calls become no-ops.
//
// printReceipt() is asynchronous: it hands the job to a background
// FreeRTOS task and returns immediately, so the UI stays responsive
// while the (slow, 9600-baud) receipt prints.
// ============================================================

class Printer {
public:
    /// Initialise the UART + printer and start the background print task.
    void begin();

    /// Queue a payment receipt for printing. Returns immediately; the
    /// receipt is rendered on a background task. `currency` is the fiat
    /// label to print (e.g. "NZD", "USD"). `payee`, when given, prints a
    /// "Paid to:" line (self-custody: the Lightning Address the money
    /// went to).
    void printReceipt(const String& merchantName,
                      const String& gstNumber,
                      const String& currency,
                      float          fiatAmount,
                      uint64_t       satAmount,
                      const String&  reference,
                      const String&  paidDate,
                      const String&  payee = "");

    bool ready() const { return _ready; }

private:
    bool _ready = false;
};

extern Printer printer;
