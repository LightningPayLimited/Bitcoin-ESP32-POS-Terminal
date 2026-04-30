# Stacked POS — ESP32-P4 Lightning Bitcoin Point of Sale

A hardware POS terminal that accepts Lightning Bitcoin payments via the [Stacked](https://stackedbitcoin.com) merchant API. Runs on the ESP32-P4-Function-EV-Board.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  ESP32-P4-Function-EV-Board                                 │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │  ILI9341    │  │  ESP32-P4    │  │  ESP32-C6-MINI-1  │  │
│  │  Touchscreen│◄─│  RISC-V     │──│  WiFi companion   │──── WiFi
│  │  (SPI/J1)   │  │  400MHz     │  │  (SDIO)           │  │
│  └─────────────┘  └──────┬───────┘  └───────────────────┘  │
│                          │ NVS                              │
│                    ┌─────┴──────┐                           │
│                    │ API key    │                           │
│                    │ WiFi creds │                           │
│                    └────────────┘                           │
└─────────────────────────────────────────────────────────────┘
                           │ HTTPS
                           ▼
              ┌────────────────────────┐
              │  app.stackedbitcoin.com │
              │  Merchant API          │
              │  POST /api/merchant/   │
              │       payment          │
              │  GET  /api/merchant/   │
              │       payment          │
              └────────────────────────┘
```

## Provisioning (Getting the API Key onto the Device)

No camera needed. Three options:

### Option A: Captive Portal (default on first boot)
1. Power on the POS terminal — it creates a WiFi network `StackedPOS-Setup`
2. Connect your phone/laptop to that network
3. Browser auto-opens (or navigate to `192.168.4.1`)
4. Enter WiFi SSID, password, and your Stacked merchant API key
5. Device saves config to NVS and reboots into POS mode

### Option B: Web Serial (Stacked-hosted setup page)
1. Plug the POS into your computer via USB-C
2. Open `web/setup.html` (or the hosted version on stackedbitcoin.com)
3. Click "Connect" → select the serial port
4. Enter WiFi + API key → click "Send Configuration"
5. Device saves and reboots

### Option C: Serial Console
Send a JSON line over serial (115200 baud):
```json
{"ssid":"MyWiFi","pass":"MyPassword","apiKey":"abc123..."}
```
To factory reset, send: `RESET`

## POS Flow

```
  Boot                 Numpad              Creating
  ┌──────┐          ┌──────────┐         ┌─────────┐
  │Check │──yes────▶│Enter $NZD│──PAY──▶│POST     │
  │NVS   │          │ amount   │         │/merchant│
  └──┬───┘          └──────────┘         │/payment │
     │ no                ▲               └────┬────┘
     ▼                   │                    │
  ┌──────┐          ┌────┴─────┐         ┌───▼──────────┐
  │Setup │          │  PAID!   │◀───────│ QR Display   │
  │Portal│          │ (5 sec)  │  paid   │ Poll 1.5s    │
  └──────┘          └──────────┘         │ Refresh @60s │
                                         └──────────────┘
```

- Amount entered in **NZD** — Stacked handles NZD→sats conversion
- Invoices expire in **60 seconds** — auto-refreshed via `txRef` (up to 10x = ~10 min)
- Payment confirmed when `paidDate` is set in the status response

## Hardware Setup

**Board:** ESP32-P4-Function-EV-Board by Espressif

**Additional:** ILI9341 SPI touchscreen (320×240) wired to J1 GPIO header

| ILI9341 | GPIO | Notes |
|---------|------|-------|
| MOSI    | 11   | SPI data out |
| MISO    | 13   | SPI data in |
| SCLK    | 12   | SPI clock |
| CS      | 10   | TFT chip select |
| DC      | 9    | Data/Command |
| RST     | 8    | Reset |
| T_CS    | 7    | Touch chip select |
| VCC     | 3.3V | |
| GND     | GND  | |

Use GPIOs ≤ 36 on J1. Avoid 35/36 (bootloader sensitive).

## Building

```bash
# Install PlatformIO (VS Code extension or CLI)
# Then:
pio run -t upload
pio device monitor
```

Uses [pioarduino](https://github.com/pioarduino/platform-espressif32) fork for ESP32-P4 Arduino support.

## Project Structure

```
stacked-pos/
├── platformio.ini              # Build config
├── include/
│   ├── config.h                # Constants and pin definitions
│   ├── config_store.h          # NVS persistent config
│   ├── setup_portal.h          # Captive portal + serial provisioning
│   ├── stacked_api.h           # Stacked merchant API client
│   └── display_ui.h            # Touchscreen UI
├── src/
│   ├── main.cpp                # Boot flow + state machine
│   ├── config_store.cpp        # NVS read/write
│   ├── setup_portal.cpp        # WiFi AP + web server + serial listener
│   ├── stacked_api.cpp         # HTTPS client for Stacked API
│   └── display_ui.cpp          # Numpad, QR, success/error screens
└── web/
    └── setup.html              # Web Serial config page (host on Stacked)
```

## Stacked Merchant API Reference

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/merchant/payment` | POST | Create invoice (`amount` in NZD) or refresh (`txRef`) |
| `/api/merchant/payment?reference=...` | GET | Poll status — `paidDate` set = paid |
| `/api/merchant/profile` | GET | Merchant name + GST |
| `/api/merchant/transactions` | GET | Transaction history |

Auth: `api-key` header.

## TODO

- [x] Pin Stacked TLS root CA (currently `setInsecure()`)
- [ ] Sound via onboard ES8311 codec (cha-ching!)
- [ ] Receipt printing (thermal printer via UART)
- [ ] ESP Web Tools flash button on Stacked website
- [ ] Transaction history screen
- [x] Settings screen (WiFi reconfigure)
- [ ] NFC tap-to-pay support
