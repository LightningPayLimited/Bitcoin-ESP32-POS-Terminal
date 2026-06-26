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

// --- NFC (PN532 bit-bang I2C) on free header GPIOs ---
// Dedicated pins (NOT shared with the FT6336 touch on 15/16), so no bus
// hand-off games like the P4 needed. Using 14/21 (clean GPIOs) rather than
// 2/3 — GPIO3 is an ESP32-S3 strapping pin (JTAG sel), bad for a bit-bang bus.
// Sweep showed the module actually wired to GPIO9 + GPIO6 (the header pins are
// mislabeled vs the chip GPIOs). Auto-swap sorts SDA/SCL orientation.
#define NFC_SDA_PIN  9
#define NFC_SCL_PIN  6

// --- UART0 console (via onboard USB bridge) ---
#define UART_RX   43
#define UART_TX   44
