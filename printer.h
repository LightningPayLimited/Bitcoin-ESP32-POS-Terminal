#pragma once
#include <Arduino.h>

// ============================================================
// Thermal printer (Adafruit CSN-A2 / generic ESC-POS over TTL).
// Prints a Bitcoin payment receipt after a successful sale.
// Safe to call printReceipt() even if the printer isn't connected —
// init() will fail silently and subsequent calls become no-ops.
// ============================================================

class Printer {
public:
    /// Initialise the UART + printer. Returns true if the printer
    /// responded (or we don't care — we still print blindly).
    void begin();

    /// Print a payment receipt. Pass the values you want on paper.
    void printReceipt(const String& merchantName,
                      const String& gstNumber,
                      float          nzdAmount,
                      uint64_t       satAmount,
                      const String&  reference,
                      const String&  paidDate);

    bool ready() const { return _ready; }

private:
    bool _ready = false;
};

extern Printer printer;
