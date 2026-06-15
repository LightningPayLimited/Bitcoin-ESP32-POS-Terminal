#pragma once
// ============================================================
// Stacked POS Configuration
// ESP32-P4-Function-EV-Board — built-in 7" MIPI-DSI panel
// ============================================================

// --- Stacked Merchant API ---
// API key and WiFi creds are stored in NVS (non-volatile storage)
// and provisioned via either:
//   A) Captive portal on first boot (device creates WiFi AP)
//   B) Web Serial config page hosted on Stacked's website
//   C) Serial console commands
#define STACKED_API_BASE "https://app.stackedbitcoin.com"

// --- NVS keys ---
#define NVS_NAMESPACE     "stacked"
#define NVS_KEY_APIKEY    "api_key"
#define NVS_KEY_SSID      "wifi_ssid"
#define NVS_KEY_PASS      "wifi_pass"
#define NVS_KEY_SETUP     "setup_done"
// Payment-provider selection (added when BTCPay support landed).
// Existing devices with no value default to Stacked.
#define NVS_KEY_PROVIDER  "provider"     // "stacked" | "btcpay"
#define NVS_KEY_BTCPAY_URL "bp_url"      // BTCPay Server base URL
#define NVS_KEY_STORE_ID  "bp_store"     // BTCPay store ID (Greenfield)
#define NVS_KEY_CURRENCY  "currency"     // invoice currency, e.g. "NZD"

// Default invoice currency for BTCPay when none is supplied.
#define BTCPAY_DEFAULT_CURRENCY "NZD"

// --- Setup mode AP ---
#define SETUP_AP_SSID   "StackedPOS-Setup"
#define SETUP_AP_PASS   ""
#define SETUP_AP_IP     "192.168.4.1"

// --- Invoice timing ---
#define INVOICE_EXPIRY_SEC        60
#define INVOICE_REFRESH_BUFFER_MS 5000
#define PAYMENT_POLL_INTERVAL_MS  3000
#define MAX_INVOICE_REFRESHES     10

// --- Display: built-in MIPI-DSI panel, 480x800 portrait ---
// Panel driver IC is likely JD9365 (common 480x800 DSI panel).
// If display stays blank after boot, the driver IC may differ —
// swap DSI timing + init ops in gfx_config.h accordingly.
#define SCREEN_WIDTH    480
#define SCREEN_HEIGHT   800
#define SCREEN_ROTATION 0

// --- Guition JC4880P433 pin map (verified from JC1060P470 sibling board) ---
#define I2C_SDA_PIN  7
#define I2C_SCL_PIN  8
#define I2C_FREQ_HZ  400000

#define LCD_RST_PIN  5
#define LCD_BL_PIN   23   // backlight enable (HIGH = on)

// GT911 capacitive touch
#define TOUCH_INT_PIN  21
#define TOUCH_RST_PIN  3
#define GT911_ADDR     0x5D  // alt: 0x14 if INT held low during reset

// --- Colours (RGB565) ---
#define COL_BG         0x0000
#define COL_FG         0xFFFF
#define COL_ACCENT     0x1575  // Stacked teal (#14afac)
#define COL_SUCCESS    0x07E0
#define COL_ERROR      0xF800
#define COL_KEYPAD_BG  0x2104
#define COL_KEYPAD_FG  0xFFFF
#define COL_HEADER_BG  0x1575
#define COL_DIM        0x8410
#define COL_BTC        0xFD20  // Bitcoin orange (~#F7931A)
#define COL_BTC_RIM    0xC2A0  // darker rim for coin edge

// --- Screensaver ---
// Show the animated splash after this much inactivity on the numpad.
#define SCREENSAVER_TIMEOUT_MS  60000

// --- QR ---
// Bolt11 Lightning invoices are ~300-600 chars alphanumeric.
// QR version 15 at ECC L holds up to 520 alphanumeric chars.
#define QR_VERSION     15
#define QR_ECC_LEVEL   0

// --- Timers ---
#define PAID_DISPLAY_MS   5000
#define ERROR_DISPLAY_MS  5000

// --- Factory reset button ---
// Hold this GPIO low for FACTORY_RESET_HOLD_MS to wipe NVS and reboot.
// GPIO 35 is the default BOOT button on most ESP32-P4 dev boards.
// If this is wrong for the JC4880P443, try 0, 22, or 36.
#define FACTORY_RESET_PIN       35
#define FACTORY_RESET_HOLD_MS   5000

// --- NFC reader (PN532 on the CN3 SH1.0 I2C connector) ---
// CN3 turned out to be the ES_I2C / system bus — the SAME GPIO7/8 the GT911
// touch panel and ES8311 codec live on. The PN532 is bit-banged here and
// time-shares the pins with the hardware-Wire touch driver (see nfc.cpp:
// releaseBusToWire). SDA=7, SCL=8 (confirmed by a live firmware-version read).
// (Was GPIO32/28 on flying leads; CN3 lets us drop the flying wires.)
#define NFC_SDA_PIN  I2C_SDA_PIN   // 7
#define NFC_SCL_PIN  I2C_SCL_PIN   // 8

// --- NFC bus probe (diagnostic) ---
// When 1, the firmware bit-bangs a quick I2C address probe at boot — BEFORE
// the touch bus starts — looking for the PN532 (addr 0x24) on candidate pin
// pairs, and shows the result on screen + serial. Used to discover which
// GPIOs the SH1.0 / CN3 I2C connector is wired to. Set to 0 for production.
// CN3 resolved to GPIO7/8 (shared ES_I2C bus) — probe retired.
#define NFC_BUS_PROBE 0
// IRQ + RST aren't wired on the 4-pin module. Adafruit's lib needs
// concrete pin numbers though — pick any free pad. These won't be driven.
#define NFC_IRQ_PIN  36
#define NFC_RST_PIN  37

// --- Thermal printer (CSN-A2 over TTL UART) ---
// Wire ESP32 TX -> printer RX (yellow), printer TX -> ESP32 RX (green),
// printer RTS -> ESP32 RTS pin (used as a CTS input — the printer pulls
// this line high when its buffer is full so we don't overrun it during
// big bitmap dumps).
// Printer needs its own 5-9V / 2A supply with shared GND.
#define PRINTER_TX_PIN  33   // ESP32 -> printer (printer's yellow wire)
#define PRINTER_RX_PIN  31   // printer -> ESP32 (printer's green wire)
#define PRINTER_RTS_PIN 30   // printer RTS -> ESP32 (acts as CTS input)
#define PRINTER_BAUD    9600
