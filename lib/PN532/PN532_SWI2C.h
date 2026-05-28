#ifndef __PN532_SWI2C_H__
#define __PN532_SWI2C_H__

#include "PN532Interface.h"
#include <Arduino.h>

// Software (bit-banged) I2C transport for the PN532.
//
// Why this exists: on the ESP32-P4 the Arduino "i2c-ng" hardware driver can't
// ride out the PN532's clock stretching — it floods ESP_ERR_INVALID_STATE on
// any command longer than getFirmwareVersion (SAMConfig, card polling, ...).
// Bit-banging lets us simply wait for the slave to release SCL on every clock
// edge, so arbitrary stretching just works. Slower than hardware I2C, but the
// PN532 is not a throughput-sensitive device.
//
// Open-drain is emulated with pinMode: release = INPUT_PULLUP (line floats
// high via pull-ups), drive-low = OUTPUT LOW.
class PN532_SWI2C : public PN532Interface {
public:
    PN532_SWI2C(uint8_t sdaPin, uint8_t sclPin);

    void begin();
    void wakeup();
    int8_t  writeCommand(const uint8_t *header, uint8_t hlen,
                         const uint8_t *body = 0, uint8_t blen = 0);
    int16_t readResponse(uint8_t buf[], uint8_t len, uint16_t timeout);

private:
    uint8_t _sda, _scl;
    uint8_t command;

    // line control (open-drain emulation)
    void    sclHigh();          // release SCL and wait out clock stretching
    void    sclLow();
    void    sdaHigh();
    void    sdaLow();
    uint8_t sdaRead();

    // I2C bit-bang primitives
    void    i2cStart();
    void    i2cStop();
    void    writeBit(uint8_t b);
    uint8_t readBit();
    uint8_t writeByte(uint8_t b);   // returns slave ACK bit (0 = ACK)
    uint8_t readByte();             // 8 data bits, no ack sent
    void    ackBit(bool ack);       // master sends ACK(0)/NACK(1)

    int8_t  readAckFrame();
};

#endif
