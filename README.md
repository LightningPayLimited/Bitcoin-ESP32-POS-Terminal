# Lightning Pay POS — ESP32-P4 Lightning Bitcoin Point of Sale

A hardware POS terminal that accepts Lightning Bitcoin payments — by QR code or
Boltcard tap-to-pay — and prints receipts. Backends: the
[Stacked](https://stackedbitcoin.com) merchant API, any
[BTCPay Server](https://btcpayserver.org) (Greenfield API), or **self-custody**
— your own wallet's Lightning Address, with no intermediary at all. Runs on the
Guition JC4880P443, an ESP32-P4 board with a built-in 4.3" MIPI-DSI touchscreen.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Guition JC4880P443                                              │
│  ┌──────────────┐   ┌──────────────┐   ┌───────────────────┐    │
│  │ 4.3" 480x800 │   │  ESP32-P4    │   │  ESP32-C6         │    │
│  │ ST7701 DSI   │◄──│  RISC-V      │───│  WiFi companion   │──── WiFi
│  │ + GT911 touch│   │  400MHz      │   │  (ESP-Hosted)     │    │
│  └──────────────┘   └──┬────┬───┬──┘   └───────────────────┘    │
│   (landscape 800x480)  │    │   │ NVS: WiFi creds, provider,    │
│  ┌──────────────┐      │    │   │      API key, store, currency │
│  │ PN532 NFC    │◄─────┘    │                                   │
│  │ (CN3, I2C    │           │ UART                              │
│  │  GPIO 7/8)   │    ┌──────┴───────┐                           │
│  └──────────────┘    │ CSN-A2       │                           │
│                      │ thermal      │                           │
│                      │ printer      │                           │
│                      └──────────────┘                           │
└──────────────────────────────────────────────────────────────────┘
                 │ HTTPS
                 ▼
   ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────────┐
   │ app.stackedbitcoin   │  │ Your BTCPay Server   │  │ Your wallet's Lightning  │
   │ /api/merchant/…      │or│ Greenfield API       │or│ Address (LNURL-pay)      │
   │ payment/transactions │  │ /api/v1/stores/…     │  │ .well-known/lnurlp/<you> │
   └──────────────────────┘  └──────────────────────┘  │ callback → verify (LUD-21)│
                                                       └──────────────────────────┘
```

The POS state machine talks to a common `PaymentProvider` interface
(`payment_provider.h`); the concrete backend — `StackedAPI`, `BTCPayAPI` or
`LnAddressAPI` — is selected at boot from the provisioned config.

## Provisioning (Getting Credentials onto the Device)

No camera needed. Three options:

### Option A: Captive Portal (default on first boot)
1. Power on the POS terminal — it creates a WiFi network `LP-POS-Setup-xxxx`
   (random suffix so multiple devices can be provisioned side by side)
2. Connect your phone/laptop to that network
3. Browser auto-opens (or navigate to `192.168.4.1`)
4. Pick a provider, then:
   - **Stacked**: enter WiFi SSID, password, and your merchant API key
   - **BTCPayServer**: enter WiFi, server URL, and a Greenfield API key
     (needs `btcpay.store.cancreateinvoice` + `btcpay.store.canviewinvoices`);
     after reboot the device lists your stores on screen — tap one to select it
   - **Self-custody (Lightning Address)**: enter WiFi, your Lightning Address
     (`you@walletprovider.com`), the currency to price in (or `SATS`), and an
     optional store name for the splash screen and receipts. No API key. See
     [Self-custody mode](#self-custody-mode-lightning-address) for which
     wallets work.
5. Device saves config to NVS and reboots into POS mode

### Option B: Web Serial (hosted setup page)
1. Plug the POS into your computer via USB-C
2. Open `setup.html` (or the hosted version)
3. Click "Connect" → select the serial port
4. Enter the config → click "Send Configuration"
5. Device saves and reboots

### Option C: Serial Console
Send a JSON line over serial (115200 baud):
```json
{"ssid":"MyWiFi","pass":"MyPassword","apiKey":"abc123..."}
```
BTCPay and self-custody use the same channel with a `provider` field:
```json
{"ssid":"MyWiFi","pass":"MyPassword","provider":"btcpay","btcpayUrl":"https://btcpay.example.com","apiKey":"...","currency":"NZD"}
{"ssid":"MyWiFi","pass":"MyPassword","provider":"lnaddress","lnAddress":"you@walletprovider.com","currency":"NZD","storeName":"My Cafe"}
```
(`storeName` is optional; `currency` defaults to NZD. The same JSON works on
`POST http://192.168.4.1/api/config` in setup mode.)

### Factory Reset
Send `RESET` over serial, or hold the BOOT button (GPIO 35) for 5 seconds.
If WiFi fails to connect, the device falls back to the portal with the saved
SSID pre-filled so you can fix it.

## POS Flow

```
  Boot                  Numpad                 Creating
  ┌──────┐          ┌────────────┐          ┌──────────┐
  │Check │──yes────▶│Enter fiat  │──PAY───▶│ Create   │
  │NVS   │          │ amount     │          │ invoice  │
  └──┬───┘          └────────────┘          └────┬─────┘
     │ no             ▲   ▲    │ menu            │
     ▼                │   │    ▼                 ▼
  ┌──────┐        ┌───┴──┐ │ ┌─────────┐   ┌──────────────┐
  │Setup │        │PAID! │ │ │ Txn     │   │ QR display   │
  │Portal│        │+print│ │ │ history │   │ + NFC poll   │
  └──────┘        │(5s)  │ │ └─────────┘   │ Poll 3s      │
                  └──────┘ │               │ Refresh @60s │
                           │ 60s idle      │ Cancel ──────┼──▶ back to numpad
                     ┌─────┴──────┐        └──────────────┘
                     │Screensaver │
                     └────────────┘
```

- Amount is entered in the configured fiat currency (NZD for Stacked; whatever
  you chose for BTCPay / self-custody) — Stacked and BTCPay convert fiat→sats
  server-side; self-custody uses the public spot rate (see below). Choosing
  `SATS` as the currency enters sats directly with no conversion.
- Invoices expire in **60 seconds** — auto-refreshed (up to 10x = ~10 min);
  BTCPay can't refresh in place, so a fresh invoice is created instead.
  Self-custody keeps each wallet invoice up for **5 minutes** (rate locked,
  like a BTCPay checkout) before requesting a fresh one; every sale still
  times out at 10 minutes of wall time (`SALE_TIMEOUT_MS`)
- Payment is confirmed by polling every 3 s; PAID screen shows until tapped and
  a receipt prints automatically (if a printer is connected)

### NFC tap-to-pay (Boltcard)

While the QR is showing, the PN532 polls for a card. On tap it reads the
NDEF LNURL-withdraw URL, resolves it, and submits the active bolt11 to the
LNURLW callback — settlement is then detected by the normal payment poll.

### Transaction history

The menu button on the numpad opens a takings screen: timeframe tabs
(24h / week / month / last month, computed in NZ local time), a scrollable
list of transactions with count and paid total, and a per-row **Check**
button that re-queries the invoice's live status. (Stacked only — BTCPay and
self-custody show a "not supported" notice.) The top-right **Update** button
opens the firmware update menu (see Firmware Updates below).

## Self-custody mode (Lightning Address)

Pick **Self-custody** at setup and the POS talks straight to the wallet
behind your Lightning Address — no merchant account, no API key, funds land
in your wallet. Under the hood it is a plain LNURL-pay client:

```
first  GET https://<domain>/.well-known/lnurlp/<you>          (LUD-16 → LUD-06)
boot   → { callback, minSendable, maxSendable, commentAllowed, metadata }
       GET callback?amount=<1 sat>  — probe: must return `verify` (LUD-21)

sale   spot rate (Coinbase, else CoinGecko)  →  fiat → sats
       GET callback?amount=<msat>&comment=<store $12.50 NZD>
       → { pr: "lnbc…", verify: "https://…" }
       show QR (bolt11)  ─┐
       GET verify every 3 s ─┘  → { settled: true, preimage } → PAID + receipt
```

**Your wallet must support LUD-21 (`verify`).** Without it the POS has no way
to learn that an invoice was paid. Known to return `verify`: **Alby**,
**Coinos**, **Stacked**, **LNbits**. **Wallet of Satoshi does not.** The POS
checks this once, on the first boot after setup, by requesting one 1-sat
invoice that is never paid (some wallets list it as pending until it
expires); the result is remembered so later boots skip the probe. If the
check fails you get a screen naming the address and the reason — tap it to
go back to the setup portal, which comes back pre-filled with everything
except the WiFi password, so you only fix the address or pick another
wallet; holding BOOT 5 s still factory-resets. A corrected config sent over
USB serial while that screen is up also takes effect (the device reboots).

What the POS verifies before showing a QR: the LNURL endpoint, callback and
verify URLs are all HTTPS with certificates validated against the Mozilla
root-CA bundle built into the firmware (an on-path attacker could otherwise
swap the invoice for their own; redirects are followed but never to plain
http); the bolt11 parses and is a mainnet (`lnbc`) invoice; its amount
equals the requested amount (LUD-06 rule). A payment counts only when
`verify` reports `settled` **with** a preimage that hashes to the invoice's
payment hash, and any `pr` it echoes matches the invoice on screen.

Other details:
- Rate: `RATE_URL_PRIMARY` / `RATE_URL_FALLBACK` in `config.h` (Coinbase spot,
  CoinGecko). A tick more than 25 % away from a rate seen in the last 15 min
  is distrusted and the other source consulted; if nothing sane is
  available the recent rate is reused, else the sale errors. The rate is
  fetched at boot and at each invoice, and locked for the invoice's
  5-minute life (`LNADDR_INVOICE_EXPIRY_SEC`).
- The sale description (store name + amount) is sent as the LNURL `comment`
  when the wallet allows it, so each sale is labelled in your wallet history.
- If a customer pays the previous QR right as the POS swaps to a fresh one,
  it is still caught: the POS keeps polling that sale's earlier invoices.
  If the wallet can't be reached at swap time, the current (still valid)
  invoice simply stays on screen.
- Receipts carry the paid invoice's payment-hash prefix as **Ref** and a
  **Paid to:** line with the address, so a wrong destination is noticed.
- Currency `SATS` turns the numpad into a whole-sats keypad (no decimal
  point). Non-dollar currencies print without a symbol (`10.00 EUR`) — the
  fonts are ASCII-only.
- Boltcard tap-to-pay works as with the other providers (and the card
  server's certificate is now validated against the same CA bundle).
- No transaction-history screen (there is no server to ask) — use your wallet.

## Hardware Setup

**Board:** Guition JC4880P443 — ESP32-P4 with built-in 4.3" 480×800 ST7701
MIPI-DSI panel and GT911 capacitive touch. WiFi comes from the onboard
ESP32-C6 via ESP-Hosted (the P4 has no radio). The UI runs landscape
(800×480) — the device is mounted on its right side.

| Function | GPIO | Notes |
|----------|------|-------|
| I2C SDA / SCL | 7 / 8 | Shared ES_I2C bus: GT911 touch, ES8311 codec, PN532 |
| LCD reset | 5 | |
| LCD backlight | 23 | HIGH = on |
| Touch INT / RST | 21 / 3 | GT911 at 0x5D |
| Printer TX / RX / RTS | 33 / 31 / 30 | CSN-A2 at 9600 baud |
| Factory reset (BOOT) | 35 | Hold 5 s |

**NFC:** a 4-pin PN532 module plugs into the CN3 SH1.0 I2C connector. CN3 is
the same GPIO 7/8 bus as the touch controller, so the PN532 is driven by
bit-banged software I2C (`lib/PN532/PN532_SWI2C`) that time-shares the pins
with the hardware `Wire` touch driver. The PN532 needs solid power — flaky
supplies (USB hubs) cause silent read failures.

**Printer:** Adafruit CSN-A2 (or generic ESC-POS TTL) thermal printer. Wire
ESP32 TX→printer RX (yellow), printer TX→ESP32 RX (green), printer RTS→GPIO 30
(flow control for big bitmap dumps). Needs its own 5–9 V / 2 A supply with
shared GND. Printing runs on a background FreeRTOS task so the UI stays
responsive; everything works fine with no printer attached.

## Building

```bash
# Install PlatformIO (VS Code extension or CLI)
# Then:
pio run -t upload
pio device monitor
```

Uses the [pioarduino](https://github.com/pioarduino/platform-espressif32) fork
for ESP32-P4 Arduino support. Upload runs at 115200 — faster rates wedge this
board's native USB-Serial/JTAG.

## Firmware Updates

Every `pio run` stamps the build with the git commit (`FW_GIT_REV`) and drops
release copies into `firmware/` (gitignored):

```
firmware/
├── lightningpay-pos_v1.0.0_2026-07-08_ceb7bdf.bin           # OTA app image
├── lightningpay-pos_v1.0.0_2026-07-08_ceb7bdf_factory.bin   # full image @0x0
└── manifest.json           # all builds: version/date/commit/size/md5/sha256
```

Publish `firmware/` plus `webflash/index.html` to a static HTTPS host — the
three update paths all feed off it:

1. **Public web flasher** (`webflash/index.html`): lists every build from the
   manifest; flashes a USB-connected POS via Web Serial (esptool-js). "Update"
   keeps settings; "Full install" writes the factory image to a blank/bricked
   board (erases everything). Chrome/Edge over HTTPS only.
2. **On-device menu**: the **Update** button on the Transactions screen fetches
   `FW_MANIFEST_URL` (set it in `config.h`), lists the builds on the touch
   screen, and OTA-installs the tapped one over HTTPS (md5-verified).
3. **Local portal**: `http://<pos-ip>/update` (also `http://192.168.4.1/update`
   in setup mode) — pick a folder of builds in the browser and upload one
   directly to the device. Handy on the bench with no public hosting.

### Publishing the update site

Upload the flasher page and the firmware folder side by side, so the page
finds the manifest at `./firmware/manifest.json`:

```
https://your-host/
├── index.html              <- copy of webflash/index.html
└── firmware/               <- copy of the repo's firmware/ folder
    ├── manifest.json
    ├── lightningpay-pos_v1.0.0_2026-07-08_ceb7bdf.bin
    └── lightningpay-pos_v1.0.0_2026-07-08_ceb7bdf_factory.bin
```

Then point the devices at it: set `FW_MANIFEST_URL` in `config.h` to
`https://your-host/firmware/manifest.json` (it ships as an `example.com`
placeholder) and reflash once — after that, devices update themselves from
the site via the on-device menu.

To cut a release: bump `FW_VERSION` in `config.h`, commit, `pio run`, and
re-upload the `firmware/` folder.

### Notes & caveats

- **Web Serial needs HTTPS** (or `localhost`) and a Chromium browser
  (Chrome / Edge / Opera) — Firefox and Safari have no Web Serial.
- If the web flasher can't connect, **hold the POS's BOOT button while
  plugging in the USB cable**, then try again (the page shows this hint too).
- The web flasher runs the serial link at **115200 only** — this board's
  native USB-Serial/JTAG wedges on baud changes (same reason as
  `upload_speed` in `platformio.ini`).
- App-only USB flashes also **erase the otadata region** (`0xe000`, 8 KB) so
  the freshly written slot boots even on a device whose last OTA ran from
  the other slot — don't flash the app image with plain esptool without
  doing the same.
- The on-device updater fetches over TLS with `setInsecure()` (no CA
  pinning); the manifest md5 check catches corruption but not a determined
  MITM. Pin your hosting CA in `fw_portal.cpp` (`fetchManifest` /
  `installFromUrl`) — or switch it to `useRootCaBundle()` from
  `tls_bundle.h` if the site has a publicly trusted cert — if that matters.
- TLS certificate *validity dates* are not checked on this core
  (`CONFIG_MBEDTLS_HAVE_TIME_DATE` is off), so the bundle and the Stacked
  pin both work even before the clock syncs; an expired-but-otherwise-valid
  cert is accepted.
- The `/update` portals on the device are **unauthenticated** — same
  LAN-trust model as the setup portal.

## Project Structure

Flat layout — sources live at the repo root (`src_dir = .`).

```
ESP32-POS/
├── platformio.ini         # Build config (pioarduino, ESP32-P4)
├── config.h               # Pins, timings, colours, feature flags
├── config_store.{h,cpp}   # NVS persistent config (WiFi + provider)
├── setup_portal.{h,cpp}   # Captive portal + serial provisioning
├── fw_portal.{h,cpp}      # Firmware updates: /update portal + on-device menu
├── fw_version.py          # Build stamping + firmware/ release folder + manifest
├── webflash/index.html    # Public Web Serial flasher page (host with firmware/)
├── payment_provider.h     # Common backend interface
├── stacked_api.{h,cpp}    # Stacked merchant API client
├── btcpay_api.{h,cpp}     # BTCPay Server Greenfield client
├── lnaddress_api.{h,cpp}  # Self-custody: Lightning Address / LNURL-pay + LUD-21
├── bolt11.{h,cpp}         # BOLT11 + LNURL bech32 decoding (host-testable)
├── fiat_rate.{h,cpp}      # BTC spot rate (Coinbase / CoinGecko) for self-custody
├── tls_bundle.{h,cpp}     # Root-CA bundle for arbitrary public HTTPS hosts
├── tx_history.{h,cpp}     # Transaction history fetch + timeframe filtering
├── nfc.{h,cpp}            # PN532 card polling + NDEF URL read
├── boltcard.{h,cpp}       # LNURL-withdraw client (Boltcard payments)
├── printer.{h,cpp}        # Async thermal receipt printing
├── display_ui.{h,cpp}     # Screens: numpad, QR, history, store picker…
├── gfx_config.h           # MIPI-DSI panel + GT911 touch init/calibration
├── main.cpp               # Boot flow + POS state machine
├── splash_image.h         # Splash / screensaver bitmap
├── logo.h, pos_fonts.h, fonts/
├── lib/PN532/             # PN532 driver incl. software-I2C transport
└── setup.html             # Web Serial config page
```

## API Reference

### Stacked (auth: `api-key` header)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/merchant/payment` | POST | Create invoice (`amount` in NZD) or refresh/re-check (`txRef`) |
| `/api/merchant/payment?reference=...` | GET | Poll status — `paidDate` set = paid |
| `/api/merchant/profile` | GET | Merchant name + GST |
| `/api/merchant/transactions` | GET | History — `from`+`to` **or** `limit`+`offset`, never both |

TLS is pinned to the Stacked root CA (`stacked_ca.h`).

### BTCPay Server (auth: `Authorization: token <apiKey>`)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/v1/stores` | GET | List stores (setup store picker) |
| `/api/v1/stores/{store}/invoices` | POST | Create invoice |
| `/api/v1/stores/{store}/invoices/{id}/payment-methods` | GET | Fetch bolt11 |
| `/api/v1/stores/{store}/invoices/{id}` | GET | Poll status |

Certificates are not pinned for BTCPay (`setInsecure`) so self-hosted servers
with arbitrary certs — or plain-HTTP LAN instances — work.

### Self-custody (LNURL-pay, no auth)

| Request | Purpose |
|---------|---------|
| `GET https://<domain>/.well-known/lnurlp/<user>` | Resolve the address (LUD-16/06): callback, min/max, `commentAllowed` |
| `GET <callback>?amount=<msat>[&comment=…]` | Create invoice → `pr` + `verify` (LUD-21) |
| `GET <verify>` | Poll settlement → `settled`, `preimage` |
| `GET api.coinbase.com/v2/prices/BTC-<CUR>/spot` | Spot rate (primary) |
| `GET api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=<cur>` | Spot rate (fallback) |

All of these are validated against the built-in root-CA bundle
(`tls_bundle.h`). `https://…`, `lnurlp://…` and bech32 `lnurl1…` are accepted
in place of `user@domain`.

## TODO

- [x] Pin Stacked TLS root CA
- [x] Transaction history screen
- [x] Receipt printing (thermal printer via UART)
- [x] NFC tap-to-pay support (Boltcard / LNURL-withdraw)
- [x] BTCPay Server backend
- [x] Screensaver + splash branding
- [x] Cancel button on the invoice screen
- [x] Landscape UI
- [x] Self-custody mode (Lightning Address + LUD-21 verify)
- [ ] Sound via onboard ES8311 codec (cha-ching!)
- [ ] ESP Web Tools flash button
- [ ] Settings screen (WiFi reconfigure without factory reset)
