#pragma once
#include <Arduino.h>

// ============================================================
// NFC reader (PN532 over I2C). Lives on the same bus as the GT911
// touch controller. Polls for ISO14443A tags and reads the URL out
// of NDEF-formatted cards (e.g. Boltcards).
//
// Boltcard flow (target):
//   1. Customer taps card -> we read NDEF URL
//   2. URL is an LNURL-Withdraw template -> resolve it
//   3. Send the active bolt11 to the LNURLW callback
//   4. Stacked detects payment via normal polling -> PAID
// ============================================================

class NFC {
public:
    /// Initialise the chip. Returns true if the PN532 responded.
    bool begin();

    /// Try to bring the PN532 up on a specific SDA/SCL pair (definitive —
    /// performs a real firmware-version read, not just an address ACK).
    /// Used by the bus probe to confirm candidate pins. On success the driver
    /// is left configured on these pins and ready() becomes true.
    bool tryPins(int sda, int scl);

    /// Poll for a card. Non-blocking (uses a short timeout).
    /// Returns true if a card is present and `outUid` / `outNdefUrl`
    /// have been populated. `outNdefUrl` may be empty if the tag
    /// isn't NDEF-formatted (still useful — we got a UID).
    bool readCard(String& outUid, String& outNdefUrl);

    bool ready() const { return _ready; }

private:
    bool _ready = false;
    uint32_t _lastReadMs = 0;
};

extern NFC nfc;
