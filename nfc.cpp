#include "nfc.h"
#include "config.h"
#include <PN532_SWI2C.h>
#include <PN532.h>

// PN532 over a bit-banged software I2C bus. The ESP32-P4's hardware i2c-ng
// driver can't ride the chip's clock stretching (floods ESP_ERR_INVALID_STATE
// on SAMConfig / card polling), so we drive SDA/SCL in software and wait out
// the stretch. Same two header pins as before: SDA=NFC_SDA_PIN, SCL=NFC_SCL_PIN.
static PN532_SWI2C* pn532i2c = nullptr;
static PN532*       pn532     = nullptr;

NFC nfc;

bool NFC::begin() {
    // Resting line levels before we start driving the bus. Both should read
    // HIGH on a healthy idle bus; a line stuck at 0 means it's held low
    // externally (e.g. a dead/under-powered module).
    pinMode(NFC_SDA_PIN, INPUT_PULLUP);
    pinMode(NFC_SCL_PIN, INPUT_PULLUP);
    delay(5);
    Serial.printf("[NFC] Idle line levels (INPUT_PULLUP): SDA(GPIO%d)=%d SCL(GPIO%d)=%d  (1=high/healthy, 0=held low)\n",
                  NFC_SDA_PIN, digitalRead(NFC_SDA_PIN),
                  NFC_SCL_PIN, digitalRead(NFC_SCL_PIN));

    pn532i2c = new PN532_SWI2C(NFC_SDA_PIN, NFC_SCL_PIN);
    pn532    = new PN532(*pn532i2c);
    pn532->begin();

    uint32_t ver = pn532->getFirmwareVersion();
    if (!ver) {
        Serial.println("[NFC] PN532 not responding to firmware-version query");
        Serial.println("[NFC] Check: DIP switches in I2C mode (1=ON,2=OFF)? Power LED on?");
        _ready = false;
        return false;
    }
    Serial.printf("[NFC] PN532 chip=0x%02X fw=%d.%d via bit-bang I2C (SDA=%d SCL=%d)\n",
                  (uint8_t)((ver >> 24) & 0xFF),
                  (uint8_t)((ver >> 16) & 0xFF),
                  (uint8_t)((ver >> 8)  & 0xFF),
                  NFC_SDA_PIN, NFC_SCL_PIN);

    pn532->setPassiveActivationRetries(0x05);
    pn532->SAMConfig();
    _ready = true;
    return true;
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

    if (millis() - _lastReadMs < 200) return false;
    _lastReadMs = millis();

    uint8_t uid[10];
    uint8_t uidLen = 0;
    if (!pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A,
                                    uid, &uidLen, 50)) return false;

    char hex[24] = {0};
    for (uint8_t i = 0; i < uidLen && i < 10; i++) {
        snprintf(hex + i * 2, 3, "%02X", uid[i]);
    }
    outUid = String(hex);
    outNdefUrl = "";

    // Try Type-4 first (NTAG424 / Boltcard — these run in privacy mode with a
    // random UID each tap, and their URL is only reachable over ISO-DEP). Done
    // immediately after activation so a failed Type-2 read can't deselect the
    // card first.
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
    return true;
}
