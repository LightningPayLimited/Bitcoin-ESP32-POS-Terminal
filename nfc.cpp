#include "nfc.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_PN532.h>

// PN532 (RDM8800) on Wire1 — separate I2C bus from the touch controller.
static Adafruit_PN532* pn532 = nullptr;

NFC nfc;

static void i2cScan(TwoWire& bus) {
    Serial.print("[I2C] devices: ");
    int count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0) {
            Serial.printf("0x%02X ", addr);
            count++;
        }
    }
    Serial.printf(" (%d total)\n", count);
}

bool NFC::begin() {
    Wire1.end();
    if (!Wire1.begin(NFC_SDA_PIN, NFC_SCL_PIN, 100000)) {
        Serial.printf("[NFC] Wire1.begin(%d,%d) failed\n",
                      NFC_SDA_PIN, NFC_SCL_PIN);
        return false;
    }
    delay(20);

    Serial.printf("[NFC] Scanning Wire1 (SDA=%d SCL=%d): ",
                  NFC_SDA_PIN, NFC_SCL_PIN);
    i2cScan(Wire1);

    pn532 = new Adafruit_PN532(NFC_IRQ_PIN, NFC_RST_PIN, &Wire1);
    pn532->begin();

    uint32_t ver = pn532->getFirmwareVersion();
    if (!ver) {
        Serial.println("[NFC] PN532 not responding to firmware-version query");
        Serial.println("[NFC] Check: module mode jumpers set to I2C? Power LED on?");
        _ready = false;
        return false;
    }
    Serial.printf("[NFC] PN532 chip=0x%02X fw=%d.%d on SDA=%d SCL=%d\n",
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
    uint8_t resp[256];
    uint8_t respLen;

    // 1) SELECT NDEF Application
    static uint8_t selectAid[] = {
        0x00, 0xA4, 0x04, 0x00, 0x07,
        0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00
    };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(selectAid, sizeof(selectAid), resp, &respLen)) return String();
    if (respLen < 2 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) return String();

    // 2) SELECT NDEF file (E1 04)
    static uint8_t selectNdef[] = { 0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x04 };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(selectNdef, sizeof(selectNdef), resp, &respLen)) return String();
    if (respLen < 2 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) return String();

    // 3) READ BINARY length (2 bytes)
    static uint8_t readLen[] = { 0x00, 0xB0, 0x00, 0x00, 0x02 };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(readLen, sizeof(readLen), resp, &respLen)) return String();
    if (respLen < 4 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) return String();
    uint16_t ndefLen = (resp[0] << 8) | resp[1];
    if (ndefLen == 0 || ndefLen > 250) return String();

    // 4) READ BINARY body
    uint8_t readBody[] = {
        0x00, 0xB0, 0x00, 0x02, (uint8_t)(ndefLen > 250 ? 250 : ndefLen)
    };
    respLen = sizeof(resp);
    if (!pn532->inDataExchange(readBody, sizeof(readBody), resp, &respLen)) return String();
    if (respLen < 2 || resp[respLen-2] != 0x90 || resp[respLen-1] != 0x00) return String();

    Serial.printf("[NFC] Type-4 NDEF (%d bytes):", respLen - 2);
    for (int i = 0; i < respLen - 2 && i < 64; i++) Serial.printf(" %02X", resp[i]);
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

    // Try Type-2 first (NTAG21x / Mifare Ultralight)
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

    // If Type-2 didn't find a URL (could be NTAG424), try Type-4
    if (outNdefUrl.length() == 0) {
        outNdefUrl = readType4Ndef();
    }
    return true;
}
