#pragma once
#include <Arduino.h>
#include <WebServer.h>

// ============================================================
// Firmware Update Portal
//
// Serves a self-contained web page at /update where new firmware
// can be flashed over WiFi (OTA, via the Update library):
//   - The page has a folder picker; every app-image .bin inside the
//     chosen folder is listed as a flash option, newest first, with
//     the most recent build preselected and tagged LATEST.
//   - The selected file is uploaded to POST /update and written to
//     the spare OTA slot; progress shows on the page AND the POS
//     display. The device reboots into the new image when done.
//
// Runs in two places:
//   - POS mode: its own WebServer on port 80 (http://<device-ip>/update)
//   - Setup mode: routes attached to the captive-portal server
//     (http://192.168.4.1/update)
//
// No auth — same LAN-trust model as the setup portal.
// ============================================================

class FirmwarePortal {
public:
    /// Register the /update routes on an existing server (used by the
    /// captive portal so updates work in setup mode too).
    static void attach(WebServer& server);

    /// Start a standalone server on port 80 for POS mode.
    void begin();

    /// Service HTTP clients. Call from loop() — non-blocking when idle.
    void handle();

    /// On-device update menu (Update button on the Transactions screen).
    /// Fetches FW_MANIFEST_URL, lists the available builds on the touch
    /// screen, and installs the tapped one over HTTPS. Blocks until the
    /// merchant backs out or an install fails; reboots on success.
    void runUpdateMenu();

private:
    WebServer _server{80};
    bool _running = false;
};
