#include "PN532_SWI2C.h"
#include "PN532_debug.h"

#define PN532_I2C_ADDRESS   0x24          // 7-bit
#define ADDR_WRITE          (PN532_I2C_ADDRESS << 1)        // 0x48
#define ADDR_READ           ((PN532_I2C_ADDRESS << 1) | 1)  // 0x49

#define HALFBIT_US          5             // ~100kHz ceiling; pinMode overhead
                                          // makes the real bus slower, which is
                                          // fine for the PN532.
#define STRETCH_TIMEOUT_US  100000UL      // 100ms max clock-stretch wait

PN532_SWI2C::PN532_SWI2C(uint8_t sdaPin, uint8_t sclPin)
    : _sda(sdaPin), _scl(sclPin), command(0) {}

// ---- line control (open-drain emulation) -------------------------------
void PN532_SWI2C::sclHigh() {
    pinMode(_scl, INPUT_PULLUP);          // release
    uint32_t t0 = micros();
    while (digitalRead(_scl) == LOW) {    // clock stretching: slave holds SCL
        if ((uint32_t)(micros() - t0) > STRETCH_TIMEOUT_US) break;
    }
}
void PN532_SWI2C::sclLow()  { pinMode(_scl, OUTPUT); digitalWrite(_scl, LOW); }
void PN532_SWI2C::sdaHigh() { pinMode(_sda, INPUT_PULLUP); }
void PN532_SWI2C::sdaLow()  { pinMode(_sda, OUTPUT); digitalWrite(_sda, LOW); }
uint8_t PN532_SWI2C::sdaRead() { pinMode(_sda, INPUT_PULLUP); return digitalRead(_sda); }

// ---- I2C bit-bang primitives -------------------------------------------
void PN532_SWI2C::i2cStart() {
    sdaHigh(); sclHigh(); delayMicroseconds(HALFBIT_US);
    sdaLow();  delayMicroseconds(HALFBIT_US);   // SDA falls while SCL high
    sclLow();  delayMicroseconds(HALFBIT_US);
}

void PN532_SWI2C::i2cStop() {
    sdaLow();  delayMicroseconds(HALFBIT_US);
    sclHigh(); delayMicroseconds(HALFBIT_US);
    sdaHigh(); delayMicroseconds(HALFBIT_US);   // SDA rises while SCL high
}

void PN532_SWI2C::writeBit(uint8_t b) {
    if (b) sdaHigh(); else sdaLow();
    delayMicroseconds(HALFBIT_US);
    sclHigh(); delayMicroseconds(HALFBIT_US);   // slave samples on rising edge
    sclLow();  delayMicroseconds(HALFBIT_US);
}

uint8_t PN532_SWI2C::readBit() {
    sdaHigh();                                  // release SDA so slave drives
    delayMicroseconds(HALFBIT_US);
    sclHigh(); delayMicroseconds(HALFBIT_US);
    uint8_t b = sdaRead();
    sclLow();  delayMicroseconds(HALFBIT_US);
    return b;
}

uint8_t PN532_SWI2C::writeByte(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) { writeBit(b & 0x80); b <<= 1; }
    return readBit();                           // ACK: 0 = ack, 1 = nack
}

uint8_t PN532_SWI2C::readByte() {
    uint8_t b = 0;
    for (uint8_t i = 0; i < 8; i++) b = (b << 1) | (readBit() ? 1 : 0);
    return b;
}

void PN532_SWI2C::ackBit(bool ack) { writeBit(ack ? 0 : 1); }

// ---- PN532Interface -----------------------------------------------------
void PN532_SWI2C::begin() {
    sclHigh();
    sdaHigh();
    delay(10);
}

void PN532_SWI2C::wakeup() {
    delay(500);   // PN532 needs time to be ready after power-up (matches lib)
}

int8_t PN532_SWI2C::writeCommand(const uint8_t *header, uint8_t hlen,
                                 const uint8_t *body, uint8_t blen) {
    command = header[0];

    i2cStart();
    writeByte(ADDR_WRITE);
    writeByte(PN532_PREAMBLE);
    writeByte(PN532_STARTCODE1);
    writeByte(PN532_STARTCODE2);

    uint8_t length = hlen + blen + 1;           // TFI + DATA
    writeByte(length);
    writeByte(~length + 1);                     // length checksum

    writeByte(PN532_HOSTTOPN532);
    uint8_t sum = PN532_HOSTTOPN532;

    for (uint8_t i = 0; i < hlen; i++) { writeByte(header[i]); sum += header[i]; }
    for (uint8_t i = 0; i < blen; i++) { writeByte(body[i]);   sum += body[i];   }

    writeByte(~sum + 1);                        // data checksum
    writeByte(PN532_POSTAMBLE);
    i2cStop();

    return readAckFrame();
}

int8_t PN532_SWI2C::readAckFrame() {
    const uint8_t ACK[6] = {0, 0, 0xFF, 0, 0xFF, 0};
    uint8_t buf[6];

    uint16_t time = 0;
    while (true) {
        i2cStart();
        writeByte(ADDR_READ);
        uint8_t status = readByte();
        if (status & 1) {                       // PN532 ready
            ackBit(true);
            for (uint8_t i = 0; i < 6; i++) {
                buf[i] = readByte();
                ackBit(i < 5);                  // ack all but the last
            }
            i2cStop();
            break;
        }
        ackBit(false);                          // not ready: NACK + retry
        i2cStop();
        delay(1);
        if (++time > PN532_ACK_WAIT_TIME) return PN532_TIMEOUT;
    }

    if (memcmp(buf, ACK, 6)) return PN532_INVALID_ACK;
    return 0;
}

int16_t PN532_SWI2C::readResponse(uint8_t buf[], uint8_t len, uint16_t timeout) {
    // Poll the leading status byte until the PN532 is ready, then read the
    // frame in the same transaction (byte-by-byte, acking until the last).
    uint16_t time = 0;
    while (true) {
        i2cStart();
        writeByte(ADDR_READ);
        uint8_t status = readByte();
        if (status & 1) { ackBit(true); break; }
        ackBit(false);
        i2cStop();
        delay(1);
        if (timeout != 0 && ++time > timeout) return PN532_TIMEOUT;
    }

    uint8_t b0 = readByte(); ackBit(true);
    uint8_t b1 = readByte(); ackBit(true);
    uint8_t b2 = readByte(); ackBit(true);
    if (b0 != 0x00 || b1 != 0x00 || b2 != 0xFF) { i2cStop(); return PN532_INVALID_FRAME; }

    uint8_t length = readByte(); ackBit(true);
    uint8_t lcs    = readByte(); ackBit(true);
    if ((uint8_t)(length + lcs) != 0) { i2cStop(); return PN532_INVALID_FRAME; }

    uint8_t tfi  = readByte(); ackBit(true);
    uint8_t rcmd = readByte(); ackBit(true);
    if (tfi != PN532_PN532TOHOST || rcmd != (uint8_t)(command + 1)) {
        i2cStop();
        return PN532_INVALID_FRAME;
    }

    length -= 2;                                // strip TFI + cmd
    if (length > len) { i2cStop(); return PN532_NO_SPACE; }

    uint8_t sum = PN532_PN532TOHOST + rcmd;
    for (uint8_t i = 0; i < length; i++) {
        buf[i] = readByte(); ackBit(true);
        sum += buf[i];
    }

    uint8_t dcs = readByte(); ackBit(true);
    if ((uint8_t)(sum + dcs) != 0) { i2cStop(); return PN532_INVALID_FRAME; }

    readByte(); ackBit(false);                  // POSTAMBLE — last byte, NACK
    i2cStop();

    return length;
}
