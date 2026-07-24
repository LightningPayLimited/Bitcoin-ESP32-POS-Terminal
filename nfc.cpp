#include "nfc.h"
#include "config.h"
#include <Wire.h>
#include <PN532_SWI2C.h>
#include <PN532.h>

// PN532 over a bit-banged software I2C bus. We bit-bang because the hardware
// I2C driver can't ride the PN532's clock stretching. The rear IIC socket sits
// on the SAME pins as the FT6336 touch (16/15), so every NFC access has to
// take the bus from the hardware Wire driver and hand it back afterwards —
// same dance as the P4.
static PN532_SWI2C* pn532i2c = nullptr;
static PN532*       pn532     = nullptr;

// Detach the hardware I2C peripheral so the bit-bang driver owns the pins.
static void busToNfc() {
    Wire.end();
}

// Give the bus back to the FT6336: reattach hardware Wire on the touch pins.
static void busToTouch() {
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);
}

NFC nfc;

// I2C bus recovery: a slave caught mid-transfer (e.g. by an MCU reset during a
// read) keeps driving SDA low forever, waiting for clocks that never come. No
// START/address write can get through until it's released. Fix: pulse the
// healthy line as a clock so the slave shifts out its remaining bits and lets
// go, then issue a STOP. Returns true if the stuck line came back up.
static bool busRecover(int stuckLine, int clockLine) {
    pinMode(stuckLine, INPUT_PULLUP);
    pinMode(clockLine, INPUT_PULLUP);
    for (int i = 0; i < 16 && digitalRead(stuckLine) == 0; i++) {
        pinMode(clockLine, OUTPUT); digitalWrite(clockLine, LOW);
        delayMicroseconds(10);
        pinMode(clockLine, INPUT_PULLUP);
        delayMicroseconds(10);
    }
    if (digitalRead(stuckLine) == 0) return false;   // still held — not a wedge
    // STOP condition: data low->high while clock is high.
    pinMode(stuckLine, OUTPUT); digitalWrite(stuckLine, LOW);
    delayMicroseconds(10);
    pinMode(stuckLine, INPUT_PULLUP);
    delayMicroseconds(10);
    return true;
}

// Try to bring up the PN532 on a specific SDA/SCL assignment. Returns true and
// leaves the driver configured + SAMConfig'd on success. begin() calls this for
// both orderings, so the connector's SDA/SCL orientation doesn't matter.
static bool tryInit(int sda, int scl) {
    // Resting line levels before we start driving the bus. Both should read
    // HIGH on a healthy idle bus; a line stuck at 0 means it's held low
    // externally — either a wedged slave (recoverable) or a dead/under-powered
    // module (not).
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    delay(5);
    if (digitalRead(sda) == 0 && digitalRead(scl) == 1) {
        Serial.printf("[NFC] GPIO%d stuck low — attempting bus recovery\n", sda);
        Serial.printf("[NFC] Recovery %s\n",
                      busRecover(sda, scl) ? "released the line" : "failed (line still held low)");
        delay(5);
    } else if (digitalRead(scl) == 0 && digitalRead(sda) == 1) {
        Serial.printf("[NFC] GPIO%d stuck low — attempting bus recovery\n", scl);
        Serial.printf("[NFC] Recovery %s\n",
                      busRecover(scl, sda) ? "released the line" : "failed (line still held low)");
        delay(5);
    }
    Serial.printf("[NFC] Try SDA=GPIO%d SCL=GPIO%d (idle: SDA=%d SCL=%d; 1=healthy)\n",
                  sda, scl, digitalRead(sda), digitalRead(scl));

    // Fresh driver objects each attempt (avoid leaking the previous try's).
    if (pn532)    { delete pn532;    pn532    = nullptr; }
    if (pn532i2c) { delete pn532i2c; pn532i2c = nullptr; }
    pn532i2c = new PN532_SWI2C(sda, scl);
    pn532    = new PN532(*pn532i2c);
    pn532->begin();

    // A PN532 waking from LowVbat can eat the first command (its wake-up ACK
    // outlasts the 10ms ACK window), so give it a few attempts.
    uint32_t ver = 0;
    for (int attempt = 0; attempt < 3 && ver == 0; attempt++) {
        if (attempt) delay(50);
        ver = pn532->getFirmwareVersion();
    }
    bool ok = (ver != 0);
    if (ok) {
        Serial.printf("[NFC] PN532 chip=0x%02X fw=%d.%d via bit-bang I2C (SDA=%d SCL=%d)\n",
                      (uint8_t)((ver >> 24) & 0xFF),
                      (uint8_t)((ver >> 16) & 0xFF),
                      (uint8_t)((ver >> 8)  & 0xFF),
                      sda, scl);
        pn532->setPassiveActivationRetries(0x05);
        pn532->SAMConfig();
    }
    return ok;
}

bool NFC::tryPins(int sda, int scl) {
    busToNfc();
    bool ok = tryInit(sda, scl);
    busToTouch();
    if (ok) _ready = true;
    return ok;
}

// Single bit-bang address probe, clock-stretch tolerant — diagnostic.
static bool bbProbe(int sda, int scl, uint8_t addr) {
    const int H = 6;
    auto rel = [](int p) { pinMode(p, INPUT_PULLUP); };
    auto low = [](int p) { pinMode(p, OUTPUT); digitalWrite(p, LOW); };
    auto sclHigh = [&]() {
        pinMode(scl, INPUT_PULLUP);
        uint32_t t0 = micros();
        while (digitalRead(scl) == 0 && micros() - t0 < 2000) {}
    };
    rel(sda); rel(scl); delayMicroseconds(H * 2);
    low(sda); delayMicroseconds(H);                    // START
    low(scl); delayMicroseconds(H);
    uint8_t b = (uint8_t)((addr << 1) | 0);
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) rel(sda); else low(sda);
        delayMicroseconds(H);
        sclHigh(); delayMicroseconds(H);
        low(scl);  delayMicroseconds(H);
        b <<= 1;
    }
    rel(sda); delayMicroseconds(H);                    // ACK slot
    sclHigh(); delayMicroseconds(H);
    bool ack = (digitalRead(sda) == 0);
    low(scl); delayMicroseconds(H);
    low(sda); delayMicroseconds(H);                    // STOP
    sclHigh(); delayMicroseconds(H);
    rel(sda); delayMicroseconds(H);
    rel(sda); rel(scl);
    return ack;
}

// Hardware-Wire single-address probe (Wire must be attached).
static bool hwProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool NFC::begin() {
    // ---- Focused bus diagnostics ----
    // 1. Baseline: is the FT6336 alive before we touch anything?
    Serial.printf("[NFC] diag: touch 0x38 via hw Wire: %s\n",
                  hwProbe(TOUCH_ADDR) ? "ACK" : "no ack");
    Serial.printf("[NFC] diag: pn532 0x24 via hw Wire: %s\n",
                  hwProbe(0x24) ? "ACK" : "no ack");

    busToNfc();
    pinMode(NFC_SDA_PIN, INPUT_PULLUP);
    pinMode(NFC_SCL_PIN, INPUT_PULLUP);
    delay(5);
    Serial.printf("[NFC] diag: idle GPIO%d=%d GPIO%d=%d\n",
                  NFC_SDA_PIN, digitalRead(NFC_SDA_PIN),
                  NFC_SCL_PIN, digitalRead(NFC_SCL_PIN));

    // 2. One bit-bang probe of 0x24 only.
    Serial.printf("[NFC] diag: pn532 0x24 bit-bang (SDA=%d SCL=%d): %s\n",
                  NFC_SDA_PIN, NFC_SCL_PIN,
                  bbProbe(NFC_SDA_PIN, NFC_SCL_PIN, 0x24) ? "ACK" : "no ack");

    // 3. Did that traffic wedge a line? Does it self-clear after 1s?
    pinMode(NFC_SDA_PIN, INPUT_PULLUP);
    pinMode(NFC_SCL_PIN, INPUT_PULLUP);
    delay(5);
    Serial.printf("[NFC] diag: post-probe idle GPIO%d=%d GPIO%d=%d",
                  NFC_SDA_PIN, digitalRead(NFC_SDA_PIN),
                  NFC_SCL_PIN, digitalRead(NFC_SCL_PIN));
    delay(1000);
    Serial.printf(" | after 1s: GPIO%d=%d GPIO%d=%d\n",
                  NFC_SDA_PIN, digitalRead(NFC_SDA_PIN),
                  NFC_SCL_PIN, digitalRead(NFC_SCL_PIN));

    // 4. Did the touch controller survive our bit-bang traffic?
    busToTouch();
    Serial.printf("[NFC] diag: touch 0x38 after bit-bang: %s\n",
                  hwProbe(TOUCH_ADDR) ? "ACK" : "no ack");

    // The IIC socket's SDA/SCL orientation isn't fixed — try the configured
    // order, then the swap. Whichever the PN532 answers on wins.
    busToNfc();
    bool ok = tryInit(NFC_SDA_PIN, NFC_SCL_PIN) ||
              tryInit(NFC_SCL_PIN, NFC_SDA_PIN);
    busToTouch();
    if (ok) {
        _ready = true;
        return true;
    }

    Serial.println("[NFC] PN532 not responding on either SDA/SCL ordering");
    Serial.println("[NFC] Check: DIP switches in I2C mode (1=ON,2=OFF)? Power LED on? IIC cable?");
    _ready = false;
    return false;
}

// NDEF URL prefix table (NFC Forum URI Record spec)
static const char* NDEF_PREFIXES[] = {
    "", "http://www.", "https://www.", "http://", "https://",
    "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.",
    "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://",
    "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:",
    "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
    "tcpobex://", "irdaobex://", "file://", "urn:epc:id:",
    "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
    "urn:nfc:"
};
static const int NDEF_PREFIX_COUNT = sizeof(NDEF_PREFIXES) / sizeof(NDEF_PREFIXES[0]);

// Parse a Type-2 NDEF TLV stream → first URL.
static String parseType2Ndef(const uint8_t* buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t t = buf[i++];
        if (t == 0x00) continue;
        if (t == 0xFE) break;
        if (i >= len) break;
        size_t l = buf[i++];
        if (l == 0xFF && i + 2 <= len) {
            l = (buf[i] << 8) | buf[i + 1];
            i += 2;
        }
        if (i + l > len) break;
        if (t != 0x03) { i += l; continue; }

        size_t end = i + l;
        while (i < end) {
            uint8_t hdr = buf[i++];
            if (i >= end) break;
            uint8_t typeLen = buf[i++];
            uint32_t payloadLen;
            if (hdr & 0x10) {
                if (i >= end) break;
                payloadLen = buf[i++];
            } else {
                if (i + 4 > end) break;
                payloadLen = ((uint32_t)buf[i] << 24) | ((uint32_t)buf[i+1] << 16)
                           | ((uint32_t)buf[i+2] << 8) | buf[i+3];
                i += 4;
            }
            uint8_t idLen = (hdr & 0x08) ? (i < end ? buf[i++] : 0) : 0;
            if (i + typeLen + idLen + payloadLen > end) break;

            bool isUrl = (typeLen == 1 && buf[i] == 'U');
            i += typeLen + idLen;

            if (isUrl && payloadLen >= 1) {
                uint8_t prefixIdx = buf[i];
                String url;
                if (prefixIdx < NDEF_PREFIX_COUNT) url = NDEF_PREFIXES[prefixIdx];
                for (uint32_t k = 1; k < payloadLen; k++) url += (char)buf[i + k];
                return url;
            }
            i += payloadLen;
        }
    }
    return String();
}

// Parse a raw NDEF *message* (no TLV wrapper) → first URL.
static String parseRawNdef(const uint8_t* msg, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t hdr = msg[i++];
        if (i >= len) return String();
        uint8_t typeLen = msg[i++];
        uint32_t payloadLen;
        if (hdr & 0x10) {
            if (i >= len) return String();
            payloadLen = msg[i++];
        } else {
            if (i + 4 > len) return String();
            payloadLen = ((uint32_t)msg[i] << 24) | ((uint32_t)msg[i+1] << 16)
                       | ((uint32_t)msg[i+2] << 8) | msg[i+3];
            i += 4;
        }
        uint8_t idLen = (hdr & 0x08) ? (i < len ? msg[i++] : 0) : 0;
        if (i + typeLen + idLen + payloadLen > len) return String();

        bool isUrl = (typeLen == 1 && msg[i] == 'U');
        i += typeLen + idLen;

        if (isUrl && payloadLen >= 1) {
            uint8_t prefixIdx = msg[i];
            String url;
            if (prefixIdx < NDEF_PREFIX_COUNT) url = NDEF_PREFIXES[prefixIdx];
            for (uint32_t k = 1; k < payloadLen; k++) url += (char)msg[i + k];
            return url;
        }
        i += payloadLen;
    }
    return String();
}

// Try Type-4 (ISO 7816, NTAG424 / Boltcard) NDEF read via PN532.
// PN532's inDataExchange handles ISO-DEP framing for us once the card is
// activated.
static String readType4Ndef() {
    if (!pn532) return String();
    // NOTE: respLen is the *input* buffer size for inDataExchange. It's a
    // uint8_t, so the buffer must be <=255 — resp[256] would overflow
    // sizeof() to 0 and make every exchange fail with NO_SPACE.
    uint8_t resp[255];
    uint8_t respLen;

    // 1) SELECT NDEF Tag Application
    static uint8_t selectAid[] = {
        0x00, 0xA4, 0x04, 0x00, 0x07,
        0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00
    };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(selectAid, sizeof(selectAid), resp, &respLen)) {
        Serial.println("[NFC] Type-4: SELECT app — exchange failed (not ISO-DEP?)");
        return String();
    }
    if (respLen < 2 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) {
        Serial.printf("[NFC] Type-4: SELECT app bad SW (%d bytes)\n", respLen);
        return String();
    }

    // 2) SELECT NDEF file (E1 04)
    static uint8_t selectNdef[] = { 0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x04 };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(selectNdef, sizeof(selectNdef), resp, &respLen)) {
        Serial.println("[NFC] Type-4: SELECT NDEF file — exchange failed");
        return String();
    }
    if (respLen < 2 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) {
        Serial.printf("[NFC] Type-4: SELECT NDEF file bad SW (%d bytes)\n", respLen);
        return String();
    }

    // 3) READ BINARY the 2-byte NLEN (NDEF message length)
    static uint8_t readLen[] = { 0x00, 0xB0, 0x00, 0x00, 0x02 };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(readLen, sizeof(readLen), resp, &respLen)) {
        Serial.println("[NFC] Type-4: READ NLEN — exchange failed");
        return String();
    }
    if (respLen < 4 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) {
        Serial.printf("[NFC] Type-4: READ NLEN bad SW (%d bytes)\n", respLen);
        return String();
    }
    uint16_t ndefLen = (resp[0] << 8) | resp[1];
    Serial.printf("[NFC] Type-4: NDEF message length = %u\n", ndefLen);
    if (ndefLen == 0 || ndefLen > 240) return String();   // 240 leaves room for SW

    // 4) READ BINARY the NDEF message (offset 2, past the NLEN field)
    uint8_t readBody[] = { 0x00, 0xB0, 0x00, 0x02, (uint8_t)ndefLen };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(readBody, sizeof(readBody), resp, &respLen)) {
        Serial.println("[NFC] Type-4: READ body — exchange failed");
        return String();
    }
    if (respLen < 2 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) {
        Serial.printf("[NFC] Type-4: READ body bad SW (%d bytes)\n", respLen);
        return String();
    }

    Serial.printf("[NFC] Type-4 NDEF (%d bytes):", respLen - 2);
    for (int i = 0; i < respLen - 2 && i < 80; i++) Serial.printf(" %02X", resp[i]);
    Serial.println();

    return parseRawNdef(resp, respLen - 2);
}

bool NFC::readCard(String& outUid, String& outNdefUrl) {
    if (!_ready || !pn532) return false;

    // Early-out before any bus activity — no hand-back needed on these paths.
    if (millis() - _lastReadMs < 200) return false;
    _lastReadMs = millis();

    // Everything below bit-bangs the shared touch bus; route to a single exit
    // so we always hand the bus back to the FT6336 afterwards.
    busToNfc();
    bool found = false;
    uint8_t uid[10];
    uint8_t uidLen = 0;
    if (pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
        char hex[24] = {0};
        for (uint8_t i = 0; i < uidLen && i < 10; i++) {
            snprintf(hex + i * 2, 3, "%02X", uid[i]);
        }
        outUid = String(hex);
        outNdefUrl = "";

        // Try Type-4 first (NTAG424 / Boltcard — privacy mode, random UID each
        // tap, URL only reachable over ISO-DEP). Done immediately after
        // activation so a failed Type-2 read can't deselect the card first.
        outNdefUrl = readType4Ndef();

        // Fall back to Type-2 (NTAG21x / Mifare Ultralight) if no Type-4 URL.
        if (outNdefUrl.length() == 0) {
            uint8_t buf[64];
            size_t got = 0;
            bool t2Ok = false;
            for (uint8_t page = 4; page <= 12 && got + 4 <= sizeof(buf); page += 4) {
                uint8_t pageBuf[16];
                if (!pn532->mifareultralight_ReadPage(page, pageBuf)) break;
                t2Ok = true;
                memcpy(buf + got, pageBuf, 16);
                got += 16;
                bool terminator = false;
                for (size_t k = 0; k < got; k++) if (buf[k] == 0xFE) { terminator = true; break; }
                if (terminator) break;
            }
            if (t2Ok && got > 0) outNdefUrl = parseType2Ndef(buf, got);
        }
        found = true;
    }

    busToTouch();
    return found;
}
