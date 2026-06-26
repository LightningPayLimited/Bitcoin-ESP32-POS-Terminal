#pragma once
#include "board_config.h"

// ============================================================
// Lightning Pay POS (ESP32-S3) — app config.
// Hardware pins live in board_config.h.
// ============================================================

#define SCREEN_WIDTH   LCD_W   // 240
#define SCREEN_HEIGHT  LCD_H   // 320

// --- Stacked Merchant API ---
#define STACKED_API_BASE "https://app.stackedbitcoin.com"

// --- NVS keys ---
#define NVS_NAMESPACE      "lightningpay"
#define NVS_KEY_APIKEY     "api_key"
#define NVS_KEY_SSID       "wifi_ssid"
#define NVS_KEY_PASS       "wifi_pass"
#define NVS_KEY_SETUP      "setup_done"
#define NVS_KEY_PROVIDER   "provider"     // "stacked" | "btcpay"
#define NVS_KEY_BTCPAY_URL "bp_url"
#define NVS_KEY_STORE_ID   "bp_store"
#define NVS_KEY_CURRENCY   "currency"
#define BTCPAY_DEFAULT_CURRENCY "NZD"

// --- Setup mode AP ---
#define SETUP_AP_SSID   "LP-POS-Setup"
#define SETUP_AP_PASS   ""
#define SETUP_AP_IP     "192.168.4.1"

// --- Colours (RGB565) — Lightning Pay palette ---
#define COL_BG         0x0000
#define COL_FG         0xFFFF
#define COL_ACCENT     0xF483   // Bitcoin orange (#F7931A)
#define COL_SUCCESS    0x07E0
#define COL_ERROR      0xF800
#define COL_KEYPAD_BG  0x2104
#define COL_KEYPAD_FG  0xFFFF
#define COL_HEADER_BG  0xF483
#define COL_DIM        0x8410

// --- NFC pin sweep (diagnostic) ---
// When 1, at boot the firmware bit-bangs an I2C address probe for the PN532
// (0x24) across all safe candidate GPIOs and reports which pair it's actually
// wired to, then halts. Use it to find the real NFC pins, then set 0.
#define NFC_PIN_SWEEP 0

// --- QR / invoice timing ---
#define QR_VERSION     15      // holds a ~300-400 char bolt11 at ECC L
#define QR_ECC_LEVEL   0       // ECC L
#define INVOICE_EXPIRY_SEC        60
#define INVOICE_REFRESH_BUFFER_MS 5000
#define PAYMENT_POLL_INTERVAL_MS  3000
#define MAX_INVOICE_REFRESHES     10
