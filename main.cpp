// ============================================================
// Stacked POS — ESP32-P4 Lightning Bitcoin Point of Sale
// ============================================================
//
// Boot flow:
//   1. Check NVS for saved config (WiFi + provider settings)
//   2. If not provisioned → start captive portal for setup
//   3. If provisioned → connect WiFi → init provider → enter POS mode
//
// POS flow (identical for every provider — see payment_provider.h):
//   1. Numpad → merchant enters a fiat amount
//   2. PAY → api->createInvoice()
//   3. Show QR (bolt11) → api->checkPayment() every 3s
//   4. Invoice expires (60s) → api->refreshInvoice()
//   5. Paid → success screen + receipt → back to numpad
//
// Providers: Stacked (merchant API), BTCPay Server (Greenfield), or
// self-custody (the merchant's own Lightning Address via LNURL-pay, with
// settlement confirmed through the LUD-21 verify URL).
//
// Provisioning:
//   - First boot: captive portal at 192.168.4.1
//   - Or: send JSON over serial: {"ssid":"...","pass":"...","apiKey":"..."}
//   - Or: Stacked webpage pushes config via Web Serial API
//   - Send "RESET" over serial to factory reset
//
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "config_store.h"
#include "setup_portal.h"
#include "fw_portal.h"
#include "display_ui.h"
#include "tx_history.h"
#include "stacked_api.h"
#include "btcpay_api.h"
#include "lnaddress_api.h"
#include "money_fmt.h"
#include "fiat_rate.h"
#include "printer.h"
#include "nfc.h"
#include "boltcard.h"

#if NFC_BUS_PROBE
#include "driver/gpio.h"                      // GPIO_IS_VALID_GPIO
#include "esp_private/esp_gpio_reserve.h"     // esp_gpio_is_reserved
#endif

// ================================================================
// State
// ================================================================
enum class State {
    BOOT,
    SETUP,             // Captive portal running
    WIFI_CONNECTING,
    IDLE,              // Numpad
    SCREENSAVER,       // Animated splash after inactivity
    CREATING_INVOICE,
    AWAITING_PAYMENT,
    PAID,
    ERROR,
    TXN_HISTORY,       // Transaction history / takings screen
};

// The loop task hosts TLS handshakes, ArduinoJson and the QR encoder's
// stack VLAs (~4.5 KB at QR_MAX_VERSION); the 8 KB default is too tight.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static State        state = State::BOOT;
static ConfigStore  config;
static SetupPortal  portal;
static FirmwarePortal fwPortal;
DisplayUI           ui;  // non-static — referenced from setup_portal.cpp

// Payment backend — one of these is selected at boot based on saved config.
static StackedAPI       stackedApi;
static BTCPayAPI        btcpayApi;
static LnAddressAPI     lnaddrApi;
static PaymentProvider* api = nullptr;

// Fiat currency label shown on the numpad + receipts (NZD for Stacked,
// the merchant-configured currency for BTCPay).
static String currencyLabel = "NZD";

static String merchantName = "";
static String enteredAmount = "";
static float  activeNzd = 0;

static MerchantInvoice activeInvoice;
static int             invoiceExpirySec = INVOICE_EXPIRY_SEC;  // per-invoice, see setActiveInvoice()
static unsigned long   invoiceCreatedAt = 0;
static unsigned long   saleStartedAt = 0;   // first invoice of this sale
static unsigned long   lastPollAt = 0;
static int             refreshCount = 0;
static bool            boltcardSubmitted = false;  // invoice handed to a Boltcard
static unsigned long   stateEnteredAt = 0;
static unsigned long   lastActivityAt = 0;

// ================================================================
// Helpers
// ================================================================
static bool checkFactoryResetButton();
static void wifiRetryWaitMs(unsigned long ms, const String& ssid);
static void resolveBTCPayStore();
static void resolveLnAddress();

// Config JSON arriving over serial after boot is saved by checkSerial();
// reboot so it actually takes effect (setup.html tells the merchant the
// terminal will restart — the captive portal already does this).
static void pollSerialConfig() {
    if (portal.checkSerial(config)) {
        Serial.println("[SYS] New config received over serial — rebooting");
        Serial.flush();
        delay(500);
        ESP.restart();
    }
}

#if NFC_BUS_PROBE
// ================================================================
// NFC bus probe — discover which GPIOs the SH1.0/CN3 I2C connector uses.
// A minimal bit-bang I2C address probe (open-drain emulated): drive a line
// LOW with OUTPUT+0, release HIGH via INPUT_PULLUP. Honours clock stretching
// by waiting for SCL to actually rise. Non-destructive — leaves both lines
// released so the hardware Wire driver can claim them afterwards.
// ================================================================
struct NfcProbeResult {
    bool ran = false, ack78 = false, ack3228 = false;
    int  nCand = 0;
    int  cand[24];
    int  foundSda = -1, foundScl = -1;
};
static NfcProbeResult nfcProbe;

// Pins already used by the board/app — skip in the beacon to cut noise.
static bool nfcKnownPin(int n) {
    switch (n) {
        case 3:  case 5:  case 7:  case 8:  case 21: case 23:   // touch/display
        case 28: case 30: case 31: case 32: case 33:            // NFC/printer
        case 35: case 36: case 37:                              // button/NFC aux
        case 24: case 25:                                       // USB-JTAG/console
            return true;
        default: return false;
    }
}

static inline void busLow(int pin)  { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
static inline void busRelease(int p){ pinMode(p, INPUT_PULLUP); }
static inline void busSclHigh(int scl) {
    pinMode(scl, INPUT_PULLUP);
    unsigned long t0 = micros();                       // honour clock stretch
    while (digitalRead(scl) == 0 && micros() - t0 < 1000) { /* wait */ }
}

// Returns true if a device ACKs its 7-bit address on these pins.
static bool i2cProbeAddr(int sda, int scl, uint8_t addr7) {
    const int H = 6;  // ~80 kHz half-period — slow and forgiving
    busRelease(sda); busRelease(scl);
    delayMicroseconds(H * 2);

    busLow(sda); delayMicroseconds(H);                 // START
    busLow(scl); delayMicroseconds(H);

    uint8_t b = (uint8_t)((addr7 << 1) | 0);           // address + write
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) busRelease(sda); else busLow(sda);
        delayMicroseconds(H);
        busSclHigh(scl); delayMicroseconds(H);
        busLow(scl);     delayMicroseconds(H);
        b <<= 1;
    }

    busRelease(sda); delayMicroseconds(H);             // 9th clock = ACK
    busSclHigh(scl); delayMicroseconds(H);
    bool ack = (digitalRead(sda) == 0);                // LOW = ACK
    busLow(scl); delayMicroseconds(H);

    busLow(sda); delayMicroseconds(H);                 // STOP
    busSclHigh(scl); delayMicroseconds(H);
    busRelease(sda); delayMicroseconds(H);

    busRelease(sda); busRelease(scl);                  // leave idle for Wire
    return ack;
}

static void runNfcBusProbe() {
    // 1) Direct address checks on the two known buses (control + CN3=7/8 case).
    Serial.println("[PROBE] Direct address check (0x24):");
    nfcProbe.ack78   = i2cProbeAddr(I2C_SDA_PIN, I2C_SCL_PIN, 0x24);
    nfcProbe.ack3228 = i2cProbeAddr(NFC_SDA_PIN, NFC_SCL_PIN, 0x24);
    Serial.printf("[PROBE]   GPIO%d/%d: %s    GPIO%d/%d: %s\n",
                  I2C_SDA_PIN, I2C_SCL_PIN, nfcProbe.ack78 ? "FOUND" : "absent",
                  NFC_SDA_PIN, NFC_SCL_PIN, nfcProbe.ack3228 ? "FOUND" : "absent");

    nfcProbe.foundSda = nfcProbe.foundScl = -1;

    // 2) Address-probe SWEEP over a safe shortlist, BOTH orderings. The module
    //    has no pull-ups (the beacon below confirms that), so the real CN3 lines
    //    won't show up as pulled-high — we must actively address them. Every pin
    //    here is known-safe to drive: ES_I2C bus (7/8), the GPIO26/27 schematic
    //    candidate, and the broken-out header GPIOs. Reserved pins are skipped.
    //    Trying both (sda,scl) orders also tells us which line is which.
    static const int CAND[] = { 7, 8, 26, 27, 29, 48, 49, 50, 51, 52 };
    const int NCAND = sizeof(CAND) / sizeof(CAND[0]);
    Serial.println("[PROBE] Sweep (both orderings) over safe candidate pins:");
    for (int i = 0; i < NCAND && nfcProbe.foundSda < 0; i++) {
        for (int j = 0; j < NCAND; j++) {
            if (i == j) continue;
            int sda = CAND[i], scl = CAND[j];
            if (esp_gpio_is_reserved((1ULL << sda) | (1ULL << scl))) continue;
            // Fast pre-filter: bare address ACK. If that bites, CONFIRM with a
            // real PN532 firmware-version read so we never report a false hit.
            if (!i2cProbeAddr(sda, scl, 0x24)) continue;
            Serial.printf("[PROBE]   ACK on SDA=%d SCL=%d — confirming...\n", sda, scl);
            if (nfc.tryPins(sda, scl)) {
                nfcProbe.foundSda = sda; nfcProbe.foundScl = scl;
                break;
            }
            Serial.println("[PROBE]   ...firmware read failed (false ACK), continuing");
        }
    }
    for (int i = 0; i < NCAND; i++) pinMode(CAND[i], INPUT);   // restore hi-Z

    if (nfcProbe.foundSda >= 0) {
        Serial.printf("[PROBE] >>> PN532 on CN3: SDA=GPIO%d  SCL=GPIO%d <<<\n",
                      nfcProbe.foundSda, nfcProbe.foundScl);
    } else {
        Serial.println("[PROBE] Sweep found nothing — running read-only beacon for info");
    }

    // 3) Read-only beacon (informational). Lists pins held high externally —
    //    these are OTHER devices' pull-ups (SDIO/codec), NOT the no-pull-up
    //    PN532. Handy for spotting the bus layout. Reading never drives a pin.
    nfcProbe.nCand = 0;
    for (int n = 0; n <= 54; n++) {
        if (!GPIO_IS_VALID_GPIO(n)) continue;
        if (esp_gpio_is_reserved(1ULL << n)) continue;
        if (nfcKnownPin(n)) continue;
        pinMode(n, INPUT_PULLDOWN);
        delayMicroseconds(80);
        bool hi = (digitalRead(n) == HIGH);
        pinMode(n, INPUT);
        if (hi && nfcProbe.nCand < 24) nfcProbe.cand[nfcProbe.nCand++] = n;
    }
    Serial.printf("[PROBE] beacon pulled-up pins: %d\n", nfcProbe.nCand);

    nfcProbe.ran = true;
}

static void showNfcBusProbe() {
    if (!nfcProbe.ran) return;
    auto* g = ui.gfx();
    g->fillScreen(COL_BG);
    g->setTextColor(COL_ACCENT, COL_BG);
    ui.setCursorTopLeft(12, 24, 3);
    g->print("NFC bus probe");

    int y = 96;
    if (nfcProbe.foundSda >= 0) {
        g->setTextColor(COL_SUCCESS, COL_BG);
        ui.setCursorTopLeft(12, y, 3); g->printf("CN3 SDA = GPIO%d", nfcProbe.foundSda); y += 46;
        ui.setCursorTopLeft(12, y, 3); g->printf("CN3 SCL = GPIO%d", nfcProbe.foundScl); y += 60;
    } else {
        g->setTextColor(COL_FG, COL_BG);
        ui.setCursorTopLeft(12, y, 2);
        g->printf("Pulled-up pins (%d):", nfcProbe.nCand); y += 34;
        String s;
        for (int i = 0; i < nfcProbe.nCand; i++) { s += "G"; s += nfcProbe.cand[i]; s += "  "; }
        if (!nfcProbe.nCand) s = "(none)";
        ui.setCursorTopLeft(12, y, 2); g->print(s); y += 44;
    }
    g->setTextColor(COL_DIM, COL_BG);
    ui.setCursorTopLeft(12, y, 2);
    g->printf("7/8:%s  32/28:%s", nfcProbe.ack78 ? "Y" : "N",
              nfcProbe.ack3228 ? "Y" : "N");
    delay(4500);
}
#endif  // NFC_BUS_PROBE

static char keyChar(Key k) {
    const char map[] = "0123456789";
    int idx = (int)k - (int)Key::D0;
    return (idx >= 0 && idx <= 9) ? map[idx] : 0;
}

void resetToIdle() {
    state = State::IDLE;
    enteredAmount = "";
    activeNzd = 0;
    refreshCount = 0;
    boltcardSubmitted = false;
    activeInvoice = {};
    if (api) api->endSale();
    lastActivityAt = millis();
    ui.showAmountEntry(enteredAmount, currencyLabel);
}

// Adopt a freshly created/refreshed invoice as the live one.
static void setActiveInvoice(const MerchantInvoice& inv) {
    activeInvoice    = inv;
    invoiceCreatedAt = millis();
    lastPollAt       = 0;
    invoiceExpirySec = inv.expirySec > 0 ? inv.expirySec : INVOICE_EXPIRY_SEC;
}

static void showActiveQR() {
    ui.showQR(activeInvoice.paymentRequest,
              activeInvoice.satAmount,
              activeInvoice.nzdAmount > 0 ? activeInvoice.nzdAmount : activeNzd,
              invoiceExpirySec,
              refreshCount,
              currencyLabel);
}

// Human-readable sale description sent to the provider (Stacked shows it
// in the dashboard; the self-custody provider passes it as the LNURL
// comment so the sale is labelled in the merchant's wallet).
static String saleDetails() {
    String amt = formatAmount(activeNzd, (uint64_t)(activeNzd + 0.5f), currencyLabel);
    return merchantName.length() > 0 ? merchantName + " " + amt : amt;
}

// Fetch + show the transaction-history screen. Blocks while paging the API
// (the "Loading..." screen is up meanwhile). Falls back to an error screen
// if there's no WiFi.
static HistoryData histData;   // held while the history screen is up

void openTransactionHistory() {
    if (WiFi.status() != WL_CONNECTED) {
        state = State::ERROR;
        stateEnteredAt = millis();
        ui.showError("No WiFi connection");
        return;
    }
    ui.showLoading("Loading transactions...");
    histData = buildHistory(*api);
    ui.resetHistoryView();
    state = State::TXN_HISTORY;
    ui.showTransactionHistory(histData, currencyLabel);
}

void handleKey(Key k) {
    // Sats mode (self-custody with currency "SATS"): whole sats only, up to
    // 7 digits (< 0.1 BTC, and exact in the float that carries the amount).
    const bool satsMode = (currencyLabel == "SATS");

    char ch = keyChar(k);
    if (ch) {
        int dot = enteredAmount.indexOf('.');
        if (dot >= 0 && (int)enteredAmount.length() - dot > 2) return;
        if (enteredAmount.length() >= (satsMode ? 7u : 10u)) return;
        if (satsMode && enteredAmount == "0") enteredAmount = "";   // no leading zeros
        enteredAmount += ch;
    } else if (k == Key::DOT) {
        if (satsMode) return;
        if (enteredAmount.indexOf('.') < 0) {
            if (enteredAmount.isEmpty()) enteredAmount = "0";
            enteredAmount += ".";
        }
    } else if (k == Key::DEL) {
        if (enteredAmount.length() > 0)
            enteredAmount.remove(enteredAmount.length() - 1);
    } else if (k == Key::CLEAR) {
        enteredAmount = "";
    } else if (k == Key::CHARGE) {
        if (satsMode) {
            unsigned long sats = strtoul(enteredAmount.c_str(), nullptr, 10);
            if (sats < 1) return;
            activeNzd = (float)sats;
        } else {
            float nzd = enteredAmount.toFloat();
            // Compare in integer cents to avoid float-precision rejection of 0.01
            int cents = (int)(nzd * 100.0f + 0.5f);
            if (cents < 1) return;  // Min 1 cent
            activeNzd = cents / 100.0f;
        }
        state = State::CREATING_INVOICE;
        return;
    } else {
        return;
    }
    ui.showAmountEntry(enteredAmount, currencyLabel);
}

// ================================================================
// Setup
// ================================================================
void setup() {
    Serial.begin(115200);
    delay(2000);  // give USB-CDC host time to (re)connect
    Serial.println("\n=============================");
    Serial.println("  Lightning Pay POS — ESP32-P4");
    Serial.println("=============================");
    Serial.flush();

    // Factory-reset button — active-low with internal pull-up
    pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

    // Thermal printer — initialises whether or not one is plugged in
    printer.begin();

#if NFC_BUS_PROBE
    // Probe for the PN532 on candidate I2C pins BEFORE the touch bus starts
    // (Wire would otherwise own GPIO 7/8). Result is shown on screen below.
    runNfcBusProbe();
#endif

    Serial.println("[BOOT] Calling ui.begin()...");
    Serial.flush();
    ui.begin();
    Serial.println("[BOOT] ui.begin() returned OK");
    Serial.flush();

#if NFC_BUS_PROBE
    showNfcBusProbe();   // paint probe result for a few seconds
#endif

    // NFC shares GPIO7/8 with the touch panel (CN3 = ES_I2C bus), so it must
    // come up AFTER ui.begin() has initialised Wire/touch. nfc.begin() bit-bangs
    // the PN532 then hands the bus back to Wire (see nfc.cpp).
    Serial.println("[BOOT] Initialising NFC (shared bus)...");
    nfc.begin();

    ui.showSplash();
    delay(1000);

    // Load config from NVS
    bool provisioned = config.begin();

    if (!provisioned) {
        // --- SETUP MODE ---
        Serial.println("[BOOT] Not provisioned — entering setup mode");
        state = State::SETUP;
        ui.showSetupInfo();

        // This blocks until config is saved, then reboots
        portal.runCaptivePortal(config);
        return;  // Won't reach here — device reboots
    }

    // --- POS MODE ---
    Serial.println("[BOOT] Config loaded — connecting WiFi...");
    Serial.printf("[WIFI] SSID='%s' pass.len=%d\n",
                  config.ssid().c_str(), config.pass().length());
    Serial.printf("[WIFI] MAC=%s\n", WiFi.macAddress().c_str());

    // Paint live WiFi status directly on the display so we can see it even
    // when USB Serial is dropping our output.
    ui.gfx()->fillScreen(COL_BG);
    ui.gfx()->setTextColor(COL_ACCENT, COL_BG);
    ui.setCursorTopLeft(10, 20, 2);
    ui.gfx()->printf("WiFi: %s", config.ssid().c_str());
    ui.setCursorTopLeft(10, 55, 2);
    ui.gfx()->printf("MAC:  %s", WiFi.macAddress().c_str());
    ui.setCursorTopLeft(10, 90, 2);
    ui.gfx()->setTextColor(COL_FG, COL_BG);
    ui.gfx()->print("Connecting...");

    static uint16_t lastReason = 0;
    static bool     gotStart = false, gotAssoc = false, gotIP = false;

    // Install verbose event logging — paint each event on the display too.
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        extern DisplayUI ui;
        auto* gfx = ui.gfx();
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_START:
                gotStart = true;
                gfx->setTextColor(0x07E0, COL_BG);  // green
                ui.setCursorTopLeft(10, 130, 2);
                gfx->print("STA started      ");
                Serial.println("[WIFI EV] STA started");
                break;
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                gotAssoc = true;
                gfx->setTextColor(0x07E0, COL_BG);
                ui.setCursorTopLeft(10, 165, 2);
                gfx->printf("Associated ch=%u ",
                            info.wifi_sta_connected.channel);
                Serial.printf("[WIFI EV] Associated ch=%u auth=%u\n",
                              info.wifi_sta_connected.channel,
                              info.wifi_sta_connected.authmode);
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                gotIP = true;
                gfx->setTextColor(0x07E0, COL_BG);
                ui.setCursorTopLeft(10, 200, 2);
                gfx->printf("IP: %s        ",
                            IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
                Serial.printf("[WIFI EV] Got IP: %s\n",
                              IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                lastReason = info.wifi_sta_disconnected.reason;
                const char* r;
                switch (lastReason) {
                    case WIFI_REASON_AUTH_EXPIRE:       r = "AUTH_EXPIRE"; break;
                    case WIFI_REASON_AUTH_FAIL:         r = "AUTH_FAIL (bad pass)"; break;
                    case WIFI_REASON_NO_AP_FOUND:       r = "NO_AP (not visible)"; break;
                    case WIFI_REASON_ASSOC_FAIL:        r = "ASSOC_FAIL"; break;
                    case WIFI_REASON_HANDSHAKE_TIMEOUT: r = "HANDSHAKE_TO"; break;
                    case WIFI_REASON_BEACON_TIMEOUT:    r = "BEACON_TO (weak)"; break;
                    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: r = "4WAY_TO"; break;
                    case 8:                             r = "ASSOC_LEAVE"; break;
                    default:                            r = "other"; break;
                }
                gfx->setTextColor(COL_ERROR, COL_BG);
                ui.setCursorTopLeft(10, 235, 2);
                gfx->printf("Disc %u %-16s", lastReason, r);
                Serial.printf("[WIFI EV] Disconnect reason=%u (%s)\n", lastReason, r);
                break;
            }
            default: break;
        }
    });

    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid().c_str(), config.pass().c_str());

    int tries = 0;
    wl_status_t last = WiFi.status();
    while (last != WL_CONNECTED && tries < 40) {
        delay(500);
        wl_status_t now = WiFi.status();
        if (now != last) {
            const char* s;
            switch (now) {
                case WL_IDLE_STATUS:     s = "IDLE"; break;
                case WL_NO_SSID_AVAIL:   s = "NO_SSID_AVAIL"; break;
                case WL_SCAN_COMPLETED:  s = "SCAN_COMPLETED"; break;
                case WL_CONNECTED:       s = "CONNECTED"; break;
                case WL_CONNECT_FAILED:  s = "CONNECT_FAILED"; break;
                case WL_CONNECTION_LOST: s = "CONNECTION_LOST"; break;
                case WL_DISCONNECTED:    s = "DISCONNECTED"; break;
                default:                 s = "?"; break;
            }
            Serial.printf("\n[WIFI] status -> %d (%s)\n", now, s);
            last = now;
        } else {
            Serial.print(".");
        }
        pollSerialConfig();
        tries++;
    }
    Serial.println();

    // If the first connect attempt failed, keep retrying forever. The
    // user has to hold the factory-reset button to wipe NVS and re-enter
    // setup mode — we no longer auto-clear config on WiFi failure.
    while (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] Failed (status=%d, RSSI=%d). Retrying...\n",
                      WiFi.status(), WiFi.RSSI());

        // Diagnostic scan — useful for understanding why the AP isn't
        // joining. Logged to serial only.
        int n = WiFi.scanNetworks(false, true);
        Serial.printf("[WIFI] Visible networks: %d\n", n);
        for (int i = 0; i < n; i++) {
            Serial.printf("  %2d) %-32s rssi=%d ch=%d auth=%d%s\n",
                          i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), (int)WiFi.encryptionType(i),
                          WiFi.SSID(i) == config.ssid() ? "  <-- target" : "");
        }
        WiFi.scanDelete();

        ui.showWifiError(config.ssid());
        wifiRetryWaitMs(5000, config.ssid());

        WiFi.disconnect(true);
        delay(200);
        WiFi.begin(config.ssid().c_str(), config.pass().c_str());

        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 40) {
            delay(500);
            pollSerialConfig();
            if (checkFactoryResetButton()) {
                ui.showWifiError(config.ssid());
            }
            tries++;
        }
    }

    Serial.printf("[BOOT] WiFi OK: %s\n", WiFi.localIP().toString().c_str());

    // Firmware update portal — http://<device-ip>/update on the LAN.
    fwPortal.begin();

    // Sync the wall-clock so the transaction-history screen can compute
    // calendar boundaries (this week / month) in NZ local time. SNTP runs in
    // the background; wait briefly for the first sync but don't block forever.
    configTzTime("NZST-12NZDT,M9.5.0,M4.1.0/3",
                 "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    for (int i = 0; i < 24 && time(nullptr) < 1700000000L; i++) delay(250);
    {
        time_t t = time(nullptr);
        Serial.printf("[BOOT] Clock: %s", t >= 1700000000L
                          ? ctime(&t) : "not synced\n");
    }

    // Init API — pick the configured payment backend.
    if (config.provider() == Provider::BTCPAY) {
        btcpayApi.begin(config.btcpayUrl(), config.apiKey(),
                        config.storeId(), config.currency());
        api = &btcpayApi;
        currencyLabel = config.currency();
        Serial.printf("[BOOT] Provider: BTCPay (%s, %s)\n",
                      config.btcpayUrl().c_str(), currencyLabel.c_str());

        // First boot after BTCPay setup: no store chosen yet. Query the
        // stores this key can access and let the merchant pick on-device.
        if (config.storeId().isEmpty()) {
            resolveBTCPayStore();
        }
    } else if (config.provider() == Provider::LNADDRESS) {
        lnaddrApi.begin(config.lnAddress(), config.currency(), config.storeName());
        api = &lnaddrApi;
        currencyLabel = config.currency();
        currencyLabel.toUpperCase();
        Serial.printf("[BOOT] Provider: Lightning Address (%s, %s)\n",
                      config.lnAddress().c_str(), currencyLabel.c_str());

        // Resolve the address and make sure the wallet can confirm
        // payments (LUD-21). Loops on the error screen until it works —
        // or parks on a "use another wallet" screen if it never can.
        resolveLnAddress();
    } else {
        stackedApi.begin(STACKED_API_BASE, config.apiKey());
        api = &stackedApi;
        currencyLabel = "NZD";
        Serial.println("[BOOT] Provider: Stacked");
    }

    // Fetch merchant profile
    ui.showLoading("Loading merchant...");
    MerchantProfile profile = api->getProfile();
    if (profile.ok) {
        merchantName = profile.companyName;
        Serial.printf("[BOOT] Merchant: %s\n", merchantName.c_str());
    }

    // Brief splash with merchant name
    ui.showSplash(merchantName);
    delay(1500);

    // Ready
    resetToIdle();
    Serial.printf("[POS] Ready — enter %s amount\n", currencyLabel.c_str());
}

// ================================================================
// Loop
// ================================================================
// Factory-reset via long-hold of FACTORY_RESET_PIN.
// Shows a countdown on screen while the button is held. Returns true
// if the countdown UI was being shown but was released before completion,
// so the caller knows to redraw whatever screen it was on.
static bool checkFactoryResetButton() {
    static unsigned long pressStart = 0;
    static int lastSecsShown = -1;

    bool pressed = (digitalRead(FACTORY_RESET_PIN) == LOW);
    if (!pressed) {
        bool wasShowingCountdown = (pressStart != 0 && lastSecsShown >= 0);
        pressStart = 0;
        lastSecsShown = -1;
        return wasShowingCountdown;
    }

    if (pressStart == 0) pressStart = millis();
    unsigned long held = millis() - pressStart;

    if (held >= FACTORY_RESET_HOLD_MS) {
        Serial.println("[SYS] Factory reset triggered — clearing NVS");
        ui.gfx()->fillScreen(COL_BG);
        ui.gfx()->setTextColor(COL_ERROR, COL_BG);
        ui.setCursorTopLeft(30, SCREEN_HEIGHT / 2 - 40, 3);
        ui.gfx()->print("FACTORY RESET");
        ui.gfx()->setTextColor(COL_DIM, COL_BG);
        ui.setCursorTopLeft(30, SCREEN_HEIGHT / 2 + 20, 2);
        ui.gfx()->print("Rebooting...");
        delay(500);
        config.clear();
        delay(500);
        ESP.restart();
    }

    int secsLeft = (FACTORY_RESET_HOLD_MS - (int)held + 999) / 1000;
    if (secsLeft != lastSecsShown) {
        lastSecsShown = secsLeft;
        ui.gfx()->fillScreen(COL_BG);
        ui.gfx()->setTextColor(COL_ACCENT, COL_BG);
        ui.setCursorTopLeft(30, 100, 3);
        ui.gfx()->print("Factory reset in");
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", secsLeft);
        // Big centred countdown digit using the existing centred-text helper
        // — easier than computing offsets for the 3x-scaled font manually.
        ui.gfx()->fillRect(0, SCREEN_HEIGHT/2 - 100, SCREEN_WIDTH, 220, COL_BG);
        ui.applyTextSize(10);  // sets font + scale; ignored for centring math
        // Use the public DisplayUI helper that already handles GFXfont metrics.
        // (drawCenteredText is private — emulate by writing through gfx().)
        {
            int16_t x1, y1; uint16_t w, h;
            ui.gfx()->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
            ui.gfx()->setTextColor(COL_ERROR, COL_BG);
            ui.gfx()->setCursor(SCREEN_WIDTH/2 - w/2 - x1,
                                SCREEN_HEIGHT/2 - h/2 - y1);
            ui.gfx()->print(buf);
        }
        ui.gfx()->setTextColor(COL_DIM, COL_BG);
        ui.setCursorTopLeft(30, SCREEN_HEIGHT - 60, 2);
        ui.gfx()->print("Release to cancel");
    }
    return false;
}

// Helper used while connecting/retrying WiFi at boot — keeps the reset
// button responsive and refreshes the error screen if the user starts
// holding the reset button and then lets go.
static void wifiRetryWaitMs(unsigned long ms, const String& ssid) {
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        pollSerialConfig();
        if (checkFactoryResetButton()) {
            ui.showWifiError(ssid);
        }
        delay(50);
    }
}

// Resolve which BTCPay store to use, the first time the device boots after
// BTCPay setup. Queries the stores the API key can access:
//   - 0 stores / unreachable → show error, keep retrying (reset button works)
//   - 1 store  → auto-select it
//   - 2+       → on-device touch picker
// The chosen store ID is persisted so this only runs once.
static void resolveBTCPayStore() {
    ui.showLoading("Finding stores...");

    std::vector<BTCPayStore> stores;
    while (true) {
        stores = btcpayApi.listStores();
        if (!stores.empty()) break;
        Serial.println("[BOOT] No BTCPay stores / unreachable — retrying");
        ui.showError("No stores found.\nCheck server/key.\nRetrying...");
        // 5s wait that keeps the factory-reset button responsive so a
        // mistyped URL/key can be wiped without a power cycle.
        unsigned long t0 = millis();
        while (millis() - t0 < 5000) {
            pollSerialConfig();
            checkFactoryResetButton();
            delay(50);
        }
    }

    int sel = 0;
    if (stores.size() == 1) {
        Serial.printf("[BOOT] Single store, auto-selecting: %s\n",
                      stores[0].name.c_str());
    } else {
        int n = (int)stores.size();
        int cap = DisplayUI::storeSelectCapacity();
        if (n > cap) {
            Serial.printf("[BOOT] %d stores; only first %d are selectable\n", n, cap);
            n = cap;
        }
        std::vector<String> names;
        for (int i = 0; i < n; i++) names.push_back(stores[i].name);

        ui.showStoreSelect(names.data(), n);
        sel = -1;
        while (sel < 0) {
            sel = ui.pollStoreSelect(n);
            pollSerialConfig();
            checkFactoryResetButton();
            delay(20);
        }
    }

    config.saveStoreId(stores[sel].id);
    btcpayApi.setStore(stores[sel].id);
    Serial.printf("[BOOT] Store selected: %s (%s)\n",
                  stores[sel].name.c_str(), stores[sel].id.c_str());
}

// Self-custody boot check.
//
// First boot after provisioning (address not yet verified): resolve the
// Lightning Address (LUD-16/06) and ask the wallet for a minimum invoice
// to confirm it returns a LUD-21 verify URL — without that the POS could
// never see a payment. Strict: loops on an error screen until it works.
//   - network / wallet failure → retry every 5 s
//   - wallet lacks verify      → parked; this wallet can't be used
// Either screen offers: tap → back to the setup portal, pre-filled with
// everything but passwords (a typo'd address shouldn't cost a factory
// reset), or hold BOOT 5 s → factory reset.
//
// Later boots (verified): resolve once, best-effort. If the wallet host
// is down the numpad still comes up and the provider re-resolves at PAY.
static void bootErrorWait(unsigned long ms, const String& title,
                          const String& reason, bool retrying) {
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        pollSerialConfig();
        if (checkFactoryResetButton()) {
            ui.showBootError(title, config.lnAddress(), reason, retrying);
        }
        if (ui.anyTouch()) {
            Serial.println("[BOOT] Tap — re-entering setup (form pre-filled)");
            ui.showLoading("Back to setup...");
            config.markUnprovisioned();
            delay(500);
            ESP.restart();
        }
        delay(50);
    }
}

static void resolveLnAddress() {
    String err;

    // A locally malformed address never reaches the network — park on it
    // instead of "retrying" a request that will never be made.
    if (!lnaddrApi.validate(err)) {
        Serial.printf("[BOOT] Bad Lightning Address: %s\n", err.c_str());
        String reason = err + ". Tap to go back to setup and fix it.";
        ui.showBootError("Bad Lightning Address", config.lnAddress(), reason, false);
        for (;;) bootErrorWait(60000, "Bad Lightning Address", reason, false);
    }

    if (config.lnVerified()) {
        ui.showLoading("Checking wallet...");
        if (!lnaddrApi.resolve(err)) {
            Serial.printf("[BOOT] Lightning Address resolve failed (%s) — "
                          "will retry at first sale\n", err.c_str());
        }
    } else {
        ui.showLoading("Checking wallet...");
        while (!lnaddrApi.resolve(err)) {
            Serial.printf("[BOOT] Lightning Address failed: %s — retrying\n", err.c_str());
            ui.showBootError("Wallet check failed", config.lnAddress(), err, true);
            bootErrorWait(5000, "Wallet check failed", err, true);
            ui.showLoading("Checking wallet...");
        }
        Serial.printf("[BOOT] Lightning Address OK: %s\n", lnaddrApi.endpoint().c_str());

        for (;;) {
            LnAddressAPI::Probe p = lnaddrApi.probeVerifySupport(err);
            if (p == LnAddressAPI::Probe::OK) {
                config.saveLnVerified();
                break;
            }
            if (p == LnAddressAPI::Probe::UNSUPPORTED) {
                Serial.printf("[BOOT] %s — this wallet can't be used\n", err.c_str());
                String reason = err + ". The POS can't confirm payments from this "
                                "wallet. Use one with LUD-21 (Alby, Coinos, Stacked, LNbits).";
                ui.showBootError("Wallet not supported", config.lnAddress(), reason, false);
                for (;;) bootErrorWait(60000, "Wallet not supported", reason, false);
            }
            Serial.printf("[BOOT] verify probe failed: %s — retrying\n", err.c_str());
            ui.showBootError("Wallet check failed", config.lnAddress(), err, true);
            bootErrorWait(5000, "Wallet check failed", err, true);
            ui.showLoading("Checking wallet...");
        }
    }

    // Warm the rate cache so the first PAY is one network call, not two.
    if (config.currency() != "SATS") {
        RateResult r = fetchBtcRate(config.currency());
        if (!r.ok) Serial.printf("[BOOT] rate warm-up failed: %s\n", r.error.c_str());
    }
}

void loop() {
    // Always check for serial commands (RESET, config JSON)
    pollSerialConfig();
    // Firmware update portal (/update) — non-blocking when idle
    fwPortal.handle();
    if (checkFactoryResetButton()) {
        // Released early — restore the appropriate screen.
        if (state == State::IDLE)             ui.showAmountEntry(enteredAmount, currencyLabel);
        else if (state == State::SCREENSAVER) ui.showScreensaver();
    }

    switch (state) {

    // --- Numpad ---
    case State::IDLE: {
        Key k = ui.pollTouch();
        if (k == Key::MENU) {
            lastActivityAt = millis();
            openTransactionHistory();
        } else if (k != Key::NONE) {
            handleKey(k);
            lastActivityAt = millis();
        } else if (millis() - lastActivityAt > SCREENSAVER_TIMEOUT_MS) {
            Serial.println("[POS] Idle — entering screensaver");
            state = State::SCREENSAVER;
            ui.showScreensaver();
        }
        break;
    }

    // --- Screensaver: static splash, tap to exit ---
    case State::SCREENSAVER: {
        if (ui.anyTouch()) {
            Serial.println("[POS] Screensaver dismissed");
            resetToIdle();
        }
        break;
    }

    // --- Create invoice ---
    case State::CREATING_INVOICE: {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[POS] PAY pressed but WiFi not connected — aborting");
            state = State::ERROR;
            stateEnteredAt = millis();
            ui.showError("No WiFi connection");
            break;
        }
        ui.showLoading("Creating invoice...");

        Serial.printf("[POS] Creating invoice %.2f %s\n", activeNzd, currencyLabel.c_str());
        MerchantInvoice created = api->createInvoice(activeNzd, saleDetails());

        if (!created.ok) {
            state = State::ERROR;
            stateEnteredAt = millis();
            ui.showError(created.error);
            break;
        }

        setActiveInvoice(created);
        saleStartedAt = millis();
        refreshCount = 0;
        boltcardSubmitted = false;
        state = State::AWAITING_PAYMENT;

        showActiveQR();

        Serial.printf("[POS] Invoice: %.2f %s = %lu sats (expiry %ds)\n",
                      activeInvoice.nzdAmount, currencyLabel.c_str(),
                      (unsigned long)activeInvoice.satAmount, invoiceExpirySec);
        break;
    }

    // --- Awaiting payment: poll + auto-refresh ---
    case State::AWAITING_PAYMENT: {
        unsigned long now = millis();
        unsigned long elapsed = now - invoiceCreatedAt;
        int secsLeft = invoiceExpirySec - (int)(elapsed / 1000);

        // Hard cap on a sale's wall time, independent of the refresh cadence
        // (self-custody invoices live minutes, not 60 s). Applies even after
        // a Boltcard accepted the invoice: the LNURL-withdraw "OK" only means
        // the wallet will *try* to pay, and the "Awaiting payment" screen
        // has no Cancel button, so this is how a failed withdrawal ends.
        if (now - saleStartedAt >= SALE_TIMEOUT_MS) {
            state = State::ERROR;
            stateEnteredAt = now;
            ui.showError("Payment timeout");
            break;
        }

        // Auto-refresh before expiry. Suspended once a Boltcard has accepted
        // the invoice — the wallet is paying this exact bolt11, so we must not
        // swap it out from under the in-flight withdrawal.
        if (!boltcardSubmitted &&
            elapsed >= (unsigned long)(invoiceExpirySec * 1000 - INVOICE_REFRESH_BUFFER_MS)) {
            // Don't mint a replacement that the cap above would kill within
            // 30 s anyway.
            if (refreshCount >= MAX_INVOICE_REFRESHES ||
                now - saleStartedAt + 30000UL >= SALE_TIMEOUT_MS) {
                state = State::ERROR;
                stateEnteredAt = millis();
                ui.showError("Payment timeout");
                break;
            }

            Serial.printf("[POS] Refreshing invoice (%d)...\n", refreshCount + 1);
            MerchantInvoice refreshed = api->refreshInvoice(activeInvoice.reference);

            // If refresh fails (e.g. old invoice expired server-side), fall
            // back to creating a brand-new invoice at the same NZD amount
            // so the customer isn't stranded with a dead QR.
            if (!refreshed.ok) {
                Serial.printf("[POS] Refresh failed (%s) — creating new invoice\n",
                              refreshed.error.c_str());
                refreshed = api->createInvoice(activeNzd, saleDetails());
            }

            if (refreshed.ok) {
                setActiveInvoice(refreshed);
                refreshCount++;
                showActiveQR();
            } else {
                Serial.printf("[POS] Invoice creation failed too: %s\n",
                              refreshed.error.c_str());
                state = State::ERROR;
                stateEnteredAt = millis();
                ui.showError("Cannot create invoice");
            }
            break;
        }

        // Poll payment status
        if (now - lastPollAt >= PAYMENT_POLL_INTERVAL_MS) {
            lastPollAt = now;
            Serial.printf("[POS] Polling (elapsed=%lus, ref='%s')\n",
                          elapsed / 1000, activeInvoice.reference.c_str());
            PaymentStatus ps = api->checkPayment(activeInvoice.reference);

            if (ps.ok && ps.isPaid) {
                state = State::PAID;
                stateEnteredAt = millis();
                float nzd = ps.nzdAmount > 0 ? ps.nzdAmount : activeNzd;
                uint64_t sats = ps.satAmount > 0 ? ps.satAmount : activeInvoice.satAmount;
                ui.showPaid(sats, nzd, currencyLabel);
                Serial.printf("[POS] PAID! %.2f %s (%lu sats) ref=%s\n",
                              nzd, currencyLabel.c_str(), (unsigned long)sats,
                              ps.reference.length() ? ps.reference.c_str()
                                                    : activeInvoice.reference.c_str());
                // The provider may report a different reference than the
                // live one (self-custody: an earlier invoice of this sale
                // settled after a refresh) — print the one that was paid.
                printer.printReceipt(merchantName, "", currencyLabel, nzd, sats,
                                     ps.reference.length() ? ps.reference
                                                           : activeInvoice.reference,
                                     ps.paidDate,
                                     api == &lnaddrApi ? lnaddrApi.payeeLabel() : String());
                break;
            }
        }

        // Update countdown (frozen once a Boltcard tap is being settled).
        if (!boltcardSubmitted && secsLeft >= 0) ui.updateTimer(secsLeft, refreshCount);

        // NFC: Boltcard tap → resolve its LNURLW and submit the active invoice.
        // On success the wallet settles this bolt11 and the poll above flips us
        // to PAID. Only submit once per invoice.
        if (!boltcardSubmitted) {
            String uid, url;
            if (nfc.readCard(uid, url)) {
                Serial.printf("[NFC] tap uid=%s url=%s\n",
                              uid.c_str(),
                              url.length() ? url.c_str() : "(no NDEF URL)");
                if (url.length()) {
                    ui.showLoading("Card detected");
                    BoltcardResult br = boltcardPay(url, activeInvoice.paymentRequest);
                    if (br.ok) {
                        boltcardSubmitted = true;
                        lastPollAt = 0;   // poll for settlement immediately
                        ui.showLoading("Awaiting payment");
                    } else {
                        Serial.printf("[POS] Boltcard declined: %s\n", br.error.c_str());
                        ui.showError(br.error.length() ? br.error : "Card declined");
                        delay(1800);
                        showActiveQR();
                    }
                }
            }
        }

        // Cancel only via the on-screen Cancel button — a stray tap elsewhere
        // (or a customer brushing the screen) no longer closes the invoice.
        if (ui.pollTouch() == Key::CANCEL) {
            Serial.println("[POS] Cancelled");
            resetToIdle();
        }
        break;
    }

    // --- Paid — stay until the merchant taps to dismiss ---
    case State::PAID: {
        if (ui.anyTouch()) resetToIdle();
        break;
    }

    // --- Transaction history — tabs/scroll handled in poll, Back returns ---
    case State::TXN_HISTORY: {
        int recIdx = -1;
        DisplayUI::HistEvent ev =
            ui.pollTransactionHistory(histData, currencyLabel, recIdx);
        if (ev == DisplayUI::HistEvent::BACK) {
            resetToIdle();
        } else if (ev == DisplayUI::HistEvent::UPDATE) {
            // Blocks in the update menu; reboots on a successful install.
            fwPortal.runUpdateMenu();
            ui.showTransactionHistory(histData, currencyLabel);
        } else if (ev == DisplayUI::HistEvent::CHECK &&
                   recIdx >= 0 && recIdx < (int)histData.all.size()) {
            // Re-check live status of the tapped transaction (button already
            // shows a spinner). Blocks briefly on the POST.
            InvoiceState st = api->checkInvoiceState(histData.all[recIdx].reference);
            ui.showCheckResult(st);
        }
        break;
    }

    // --- Error ---
    case State::ERROR: {
        if (millis() - stateEnteredAt > ERROR_DISPLAY_MS || ui.anyTouch())
            resetToIdle();
        break;
    }

    default: break;
    }

    delay(50);
}
