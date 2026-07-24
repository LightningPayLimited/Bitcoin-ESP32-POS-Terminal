#pragma once
// ============================================================
// 2.8" ESP32-S3 Display (lcdwiki) pin map.
// ILI9341V 240x320 4-line SPI LCD + FT6336 capacitive touch (I2C).
// Source: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
// ============================================================

// --- LCD: ILI9341V, 4-line SPI ---
#define LCD_CS    10
#define LCD_DC    46
#define LCD_SCK   12
#define LCD_MOSI  11
#define LCD_MISO  13
#define LCD_RST   -1    // tied to the ESP32-S3 reset line — no dedicated GPIO
#define LCD_BL    45    // backlight enable (HIGH = on)
#define LCD_W     240
#define LCD_H     320

// --- Touch: FT6336 capacitive, I2C ---
#define TOUCH_SDA  16
#define TOUCH_SCL  15
#define TOUCH_RST  18   // active low
#define TOUCH_INT  17   // active low
#define TOUCH_ADDR 0x38 // FT6x36 standard 7-bit address

// --- NFC (PN532 bit-bang I2C) on the rear IIC socket ---
// Per lcdwiki, the board's 1.25mm 4P "IIC" socket on the back is wired to
// IO16/IO15 — the SAME bus as the FT6336 touch. So the PN532 shares the touch
// bus and nfc.cpp does a Wire hand-off around each access, like the P4.
// (Earlier sweep "hit" on GPIO9+GPIO6 was a false positive: GPIO9 is the
// battery ADC divider — slow rise reads as ACK — and GPIO6 is an audio pin
// held low by the amp.)
#define NFC_SDA_PIN  TOUCH_SDA
#define NFC_SCL_PIN  TOUCH_SCL

// --- BOOT key (also the GPIO0 strapping pin) ---
// Readable as a normal active-low input once the app is running. Do NOT gate
// anything on it at power-on: held through a reset it puts the chip in the
// ROM bootloader instead of running the app.
#define BOOT_BTN  0

// --- UART0 console (via onboard USB bridge) ---
#define UART_RX   43
#define UART_TX   44
