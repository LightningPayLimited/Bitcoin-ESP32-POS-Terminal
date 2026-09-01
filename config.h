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

// --- Firmware ---
// Shown on the /update firmware portal next to the build timestamp.
// Bump when cutting a release so devices report what they're running.
#define FW_VERSION "1.0.0"

// Public site hosting the firmware/ folder that fw_version.py maintains
// (versioned .bins + manifest.json). The on-device update menu (Update
// button on the Transactions screen) fetches this manifest and installs
// the chosen build over HTTPS. TODO: point at your real hosting.
#define FW_MANIFEST_URL "https://thebitcointerminal.com/firmware/manifest.json"

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

// --- Self-custody: Lightning Address / LNURL-pay (LUD-06/16/21) ---
// The POS requests invoices straight from the merchant's own wallet via
// its Lightning Address and confirms settlement by polling the LUD-21
// `verify` URL. No API key, no intermediary.
#define NVS_KEY_LN_ADDR   "ln_addr"      // user@domain, https://…, or lnurl1…
#define NVS_KEY_LN_NAME   "ln_name"      // optional store name (splash/receipt)
#define NVS_KEY_LN_OK     "ln_ok"        // "1" once the wallet passed the LUD-21 probe
#define LNADDR_DEFAULT_CURRENCY "NZD"    // "SATS" = enter sats directly
// Wallet-issued invoices usually live for hours, so the QR is kept up for
// this long (rate locked meanwhile — BTCPay's default is 15 min) before a
// fresh, re-rated one is requested. Fewer unpaid invoices in the wallet,
// fewer QR swaps under a scanning customer. Capped by SALE_TIMEOUT_MS.
#define LNADDR_INVOICE_EXPIRY_SEC 300
#define LNADDR_BOLT11_PREFIX      "lnbc" // mainnet only
// Fiat->BTC spot rate sources for self-custody mode (no API key). %s is the
// ISO currency code; primary first, fallback on failure, then a cached
// value younger than RATE_STALE_MAX_MS.
#define RATE_URL_PRIMARY  "https://api.coinbase.com/v2/prices/BTC-%s/spot"
#define RATE_URL_FALLBACK "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=%s"
#define RATE_STALE_MAX_MS (15UL * 60UL * 1000UL)
// HTTP timeouts: resolve / callback (invoice creation) vs the 3-second
// settlement poll, which runs inside loop() and must not stall the UI.
#define LNURL_HTTP_TIMEOUT_MS  15000
#define LNADDR_POLL_TIMEOUT_MS 8000

// --- Setup mode AP ---
#define SETUP_AP_SSID   "Stacked-POS-Setup"
#define SETUP_AP_PASS   ""
#define SETUP_AP_IP     "192.168.4.1"

// --- Invoice timing ---
#define INVOICE_EXPIRY_SEC        60      // default countdown (Stacked/BTCPay)
#define INVOICE_REFRESH_BUFFER_MS 5000
#define PAYMENT_POLL_INTERVAL_MS  3000
#define MAX_INVOICE_REFRESHES     10
// A sale ends with "Payment timeout" after this many refreshes OR this
// much wall time, whichever comes first — so providers whose invoices
// live longer than 60 s (self-custody) still time out at ~10 minutes.
#define SALE_TIMEOUT_MS           (MAX_INVOICE_REFRESHES * INVOICE_EXPIRY_SEC * 1000UL)

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
#define COL_ACCENT     0x1575  // Stacked teal (#14AFAC) — brand accent
#define COL_SUCCESS    0x07E0
#define COL_ERROR      0xF800
#define COL_KEYPAD_BG  0x2104
#define COL_KEYPAD_FG  0xFFFF
#define COL_HEADER_BG  0x1575  // Stacked teal header bar
#define COL_DIM        0x8410
#define COL_BTC        0xFD20  // Bitcoin orange (~#F7931A)
#define COL_BTC_RIM    0xC2A0  // darker rim for coin edge

// --- Screensaver ---
// Show the animated splash after this much inactivity on the numpad.
#define SCREENSAVER_TIMEOUT_MS  60000

// --- QR ---
// Bolt11 Lightning invoices are ~250-600 chars (uppercased -> alphanumeric
// mode). QR version 15 at ECC L holds 758 alphanumeric chars; the renderer
// starts there and steps up to QR_MAX_VERSION when an invoice (e.g. one
// with many route hints) needs more room. v20 = 97 modules = 4 px/module
// in the 440 px QR area — still scannable.
#define QR_VERSION     15
#define QR_MAX_VERSION 20
#define QR_MAX_CHARS   1249   // alphanumeric capacity of QR_MAX_VERSION @ ECC L
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
