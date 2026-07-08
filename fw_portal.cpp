#include "fw_portal.h"
#include "config.h"
#include "display_ui.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <vector>

extern DisplayUI ui;

// Injected by fw_version.py (extra_scripts) — short git commit of the build.
#ifndef FW_GIT_REV
#define FW_GIT_REV "unknown"
#endif

// ================================================================
// Update page — self-contained, served from PROGMEM.
// The folder picker (webkitdirectory) lists every .bin in the chosen
// folder as a radio option, newest first. Known non-app images
// (bootloader/partitions) are hidden — OTA can only flash app images.
// ================================================================
static const char UPDATE_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lightning Pay Firmware Update</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #111; color: #fff;
         display: flex; justify-content: center; padding: 20px; }
  .card { background: #1a1a1a; border-radius: 16px; padding: 32px;
          max-width: 440px; width: 100%; }
  h1 { color: #f7931a; font-size: 24px; margin-bottom: 4px; }
  .sub { color: #888; font-size: 14px; margin-bottom: 20px; }
  .info { background: #222; border: 1px solid #333; border-radius: 8px;
          padding: 12px; font-size: 13px; color: #aaa; line-height: 1.6; }
  .info b { color: #ddd; font-weight: 600; }
  label.top { display: block; color: #ccc; font-size: 14px; margin: 20px 0 6px; }
  .pick { width: 100%; padding: 12px; border-radius: 8px; border: 1px dashed #444;
          background: #222; color: #f7931a; font-size: 15px; cursor: pointer; }
  .pick:hover { border-color: #f7931a; }
  .hint { color: #666; font-size: 12px; margin-top: 6px; }
  .hidden { display: none; }
  #list { margin-top: 16px; }
  .file { display: block; background: #222; border: 1px solid #333; border-radius: 8px;
          padding: 10px 12px; margin-bottom: 8px; cursor: pointer; }
  .file:hover { border-color: #555; }
  .file.sel { border-color: #f7931a; }
  .file input { margin-right: 8px; accent-color: #f7931a; }
  .fname { font-size: 14px; word-break: break-all; }
  .latest { color: #0c0; font-style: normal; font-size: 11px; font-weight: 700;
            border: 1px solid #0c0; border-radius: 4px; padding: 1px 5px; margin-left: 6px; }
  .fmeta { display: block; color: #777; font-size: 12px; margin: 4px 0 0 24px; }
  .flash { width: 100%; padding: 14px; border-radius: 8px; border: none;
           background: #f7931a; color: #000; font-size: 18px; font-weight: 700;
           cursor: pointer; margin-top: 16px; }
  .flash:hover:enabled { background: #d97e00; }
  .flash:disabled { background: #444; color: #888; cursor: default; }
  .barwrap { margin-top: 16px; height: 22px; background: #222; border: 1px solid #333;
             border-radius: 8px; overflow: hidden; }
  #bar { height: 100%; width: 0; background: #f7931a; transition: width .2s; }
  #status { margin-top: 12px; font-size: 14px; color: #aaa; min-height: 20px; }
  #status.err { color: #f55; }
  #status.ok { color: #0c0; }
</style>
</head>
<body>
<div class="card">
  <h1>&#9889; Firmware Update</h1>
  <p class="sub">Lightning Pay POS terminal</p>

  <div class="info" id="info">Loading device info&hellip;</div>

  <label class="top">Firmware folder</label>
  <button type="button" class="pick" id="pickBtn">&#128193; Choose folder&hellip;</button>
  <input type="file" id="dirInput" webkitdirectory multiple class="hidden">
  <p class="hint">Every firmware .bin in the folder is listed below, newest first.
     Bootloader/partition images are hidden &mdash; only app images can be
     flashed over the air.</p>

  <div id="list"></div>

  <button class="flash" id="flashBtn" disabled>Flash firmware</button>
  <div class="barwrap hidden" id="barwrap"><div id="bar"></div></div>
  <p id="status"></p>
</div>

<script>
const dirInput = document.getElementById('dirInput');
const pickBtn  = document.getElementById('pickBtn');
const listEl   = document.getElementById('list');
const flashBtn = document.getElementById('flashBtn');
const barwrap  = document.getElementById('barwrap');
const bar      = document.getElementById('bar');
const st       = document.getElementById('status');

let files = [];

const SKIP = ['bootloader.bin', 'partitions.bin', 'boot_app0.bin', 'ota_data_initial.bin'];
const esc = s => s.replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const fmtSize = n => n >= 1048576 ? (n/1048576).toFixed(2)+' MB' : Math.round(n/1024)+' KB';
const fmtDate = t => new Date(t).toLocaleString();

fetch('/update/info').then(r => r.json()).then(j => {
  document.getElementById('info').innerHTML =
    '<b>Current firmware:</b> v' + esc(j.version) + ' @ ' + esc(j.git) + ' &middot; built ' + esc(j.build) + '<br>' +
    '<b>App size:</b> ' + fmtSize(j.sketchSize) + ' &middot; <b>OTA space:</b> ' + fmtSize(j.freeSpace);
}).catch(() => { document.getElementById('info').textContent = 'Device info unavailable'; });

pickBtn.addEventListener('click', () => dirInput.click());
dirInput.addEventListener('change', () => loadFiles(dirInput.files));

function loadFiles(fl) {
  files = [...fl].filter(f =>
    f.name.toLowerCase().endsWith('.bin') && !SKIP.includes(f.name.toLowerCase()));
  files.sort((a, b) => b.lastModified - a.lastModified);
  render();
}

function render() {
  if (!files.length) {
    listEl.innerHTML = '<p class="hint">No firmware (.bin) files found in that folder.</p>';
    flashBtn.disabled = true;
    return;
  }
  listEl.innerHTML = files.map((f, i) =>
    '<label class="file' + (i === 0 ? ' sel' : '') + '">' +
    '<input type="radio" name="fw" value="' + i + '"' + (i === 0 ? ' checked' : '') + '>' +
    '<span class="fname">' + esc(f.webkitRelativePath || f.name) +
      (i === 0 ? '<em class="latest">LATEST</em>' : '') + '</span>' +
    '<span class="fmeta">' + fmtSize(f.size) + ' &middot; ' + fmtDate(f.lastModified) + '</span>' +
    '</label>').join('');
  listEl.querySelectorAll('input[name=fw]').forEach(r => r.addEventListener('change', () => {
    listEl.querySelectorAll('.file').forEach(el => el.classList.remove('sel'));
    r.closest('.file').classList.add('sel');
  }));
  flashBtn.disabled = false;
}

flashBtn.addEventListener('click', async () => {
  const sel = document.querySelector('input[name=fw]:checked');
  if (!sel) return;
  const f = files[+sel.value];

  // ESP32 app images start with the 0xE9 magic byte — warn on anything else.
  const head = new Uint8Array(await f.slice(0, 1).arrayBuffer());
  if (head[0] !== 0xE9) {
    if (!confirm(f.name + ' does not look like an ESP32 app image (bad magic byte). Flash anyway?')) return;
  } else if (!confirm('Flash ' + f.name + ' (' + fmtSize(f.size) + ')?\nThe POS will reboot when done.')) {
    return;
  }

  const fd = new FormData();
  fd.append('firmware', f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update?size=' + f.size);
  xhr.upload.addEventListener('progress', e => {
    if (e.lengthComputable) bar.style.width = Math.round(e.loaded / e.total * 100) + '%';
  });
  xhr.onload = () => {
    if (xhr.status === 200) { bar.style.width = '100%'; rebooting(); }
    else fail(xhr.responseText || ('HTTP ' + xhr.status));
  };
  xhr.onerror = () => fail('Connection lost during upload');

  flashBtn.disabled = true;
  pickBtn.disabled = true;
  barwrap.classList.remove('hidden');
  st.className = '';
  st.textContent = 'Uploading & flashing… do not power off the POS.';
  xhr.send(fd);
});

function fail(msg) {
  st.className = 'err';
  st.textContent = '❌ ' + msg;
  flashBtn.disabled = false;
  pickBtn.disabled = false;
}

function rebooting() {
  st.className = 'ok';
  st.textContent = '✅ Flashed! POS is rebooting…';
  setTimeout(function poll() {
    fetch('/update/info', {cache: 'no-store'}).then(r => r.json()).then(j => {
      st.textContent = '✅ Back online — v' + j.version + ' (' + j.build + ')';
    }).catch(() => setTimeout(poll, 2000));
  }, 6000);
}
</script>
</body>
</html>
)rawhtml";

// ================================================================
// On-device progress screen — painted from the upload callback so the
// merchant can see the update happening (and knows not to pull power).
// ================================================================
static int s_lastPct = -1;

static void paintUpdateStart(const String& fname) {
    auto* g = ui.gfx();
    g->fillScreen(COL_BG);
    g->setTextColor(COL_ACCENT, COL_BG);
    ui.setCursorTopLeft(24, 36, 3);
    g->print("Firmware update");
    g->setTextColor(COL_FG, COL_BG);
    ui.setCursorTopLeft(24, 110, 2);
    g->print(fname);
    g->drawRect(24, SCREEN_HEIGHT / 2 - 22, SCREEN_WIDTH - 48, 44, COL_DIM);
    g->setTextColor(COL_ERROR, COL_BG);
    ui.setCursorTopLeft(24, SCREEN_HEIGHT - 70, 2);
    g->print("Do not power off");
    s_lastPct = -1;
}

static void paintUpdatePct(int pct) {
    if (pct == s_lastPct) return;
    s_lastPct = pct;
    auto* g = ui.gfx();
    int w = (SCREEN_WIDTH - 52) * pct / 100;
    g->fillRect(26, SCREEN_HEIGHT / 2 - 20, w, 40, COL_ACCENT);
    g->setTextColor(COL_FG, COL_BG);
    ui.setCursorTopLeft(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT / 2 + 44, 3);
    g->printf("%3d%%", pct);
}

// ================================================================
// HTTP handlers
// ================================================================
static void sendInfo(WebServer& server) {
    JsonDocument doc;
    doc["version"]    = FW_VERSION;
    doc["git"]        = FW_GIT_REV;
    doc["build"]      = __DATE__ " " __TIME__;
    doc["chip"]       = ESP.getChipModel();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["freeSpace"]  = ESP.getFreeSketchSpace();
    doc["ip"]         = (WiFi.getMode() & WIFI_MODE_AP)
                            ? WiFi.softAPIP().toString()
                            : WiFi.localIP().toString();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleUploadChunk(WebServer& server) {
    HTTPUpload& up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        // The page passes the file size as ?size= so the OTA slot check and
        // the on-screen percentage use the real total (multipart uploads
        // don't expose a per-file length up front).
        size_t declared = server.hasArg("size")
                              ? (size_t)server.arg("size").toInt() : 0;
        Serial.printf("[FWUP] Receiving '%s' (%u bytes declared)\n",
                      up.filename.c_str(), (unsigned)declared);
        paintUpdateStart(up.filename);
        Update.onProgress([](size_t done, size_t total) {
            if (total) paintUpdatePct((int)(done * 100 / total));
        });
        if (!Update.begin(declared ? declared : UPDATE_SIZE_UNKNOWN)) {
            Serial.printf("[FWUP] begin failed: %s\n", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!Update.hasError() &&
            Update.write(up.buf, up.currentSize) != up.currentSize) {
            Serial.printf("[FWUP] write failed: %s\n", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[FWUP] Flashed %u bytes\n", up.totalSize);
        } else {
            Serial.printf("[FWUP] end failed: %s\n", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        // Client vanished mid-upload. The current firmware is untouched;
        // reboot to restore whatever screen the POS was on.
        Update.abort();
        Serial.println("[FWUP] Upload aborted — rebooting");
        delay(500);
        ESP.restart();
    }
}

void FirmwarePortal::attach(WebServer& server) {
    server.on("/update", HTTP_GET, [&server]() {
        server.send(200, "text/html", UPDATE_HTML);
    });

    server.on("/update/info", HTTP_GET, [&server]() {
        sendInfo(server);
    });

    server.on("/update", HTTP_POST,
        [&server]() {
            bool ok = !Update.hasError();
            server.sendHeader("Connection", "close");
            server.send(ok ? 200 : 500, "text/plain",
                        ok ? "OK" : String("Update failed: ") + Update.errorString());
            // Reboot either way: success boots the new image; failure reboots
            // the untouched current one, restoring the POS UI cleanly.
            Serial.printf("[FWUP] %s — rebooting\n", ok ? "Update OK" : "Update FAILED");
            delay(800);
            ESP.restart();
        },
        [&server]() {
            handleUploadChunk(server);
        });
}

void FirmwarePortal::begin() {
    attach(_server);
    _server.begin();
    _running = true;
    Serial.printf("[FWUP] Firmware portal: http://%s/update\n",
                  WiFi.localIP().toString().c_str());
}

void FirmwarePortal::handle() {
    if (_running) _server.handleClient();
}

// ================================================================
// On-device update menu — fetches manifest.json from the public
// firmware site (FW_MANIFEST_URL) and lists the app builds as
// tappable rows; the chosen build is downloaded over HTTPS and
// flashed to the spare OTA slot.
// ================================================================
struct FwBuild {
    String   version, commit, date, file, md5;
    uint32_t size = 0;
};

// Everything in firmware/ shares the manifest's directory.
static String manifestBaseUrl() {
    String u = FW_MANIFEST_URL;
    int slash = u.lastIndexOf('/');
    return slash > 0 ? u.substring(0, slash + 1) : u;
}

// NOTE: setInsecure() — the firmware site's CA isn't pinned (same trade-off
// as boltcard.cpp). The manifest's md5 is verified before the new image is
// accepted, which catches corruption but not a determined MITM; pin the
// site's CA here if that matters for your deployment.
static bool fetchManifest(std::vector<FwBuild>& out, String& err) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, FW_MANIFEST_URL)) { err = "Bad manifest URL"; return false; }
    int code = http.GET();
    if (code != 200) {
        err = "Manifest HTTP " + String(code);
        http.end();
        return false;
    }
    JsonDocument doc;
    DeserializationError je = deserializeJson(doc, http.getString());
    http.end();
    if (je) { err = "Manifest parse error"; return false; }

    for (JsonObject b : doc["builds"].as<JsonArray>()) {
        JsonObject app = b["app"];
        if (app.isNull()) continue;
        FwBuild fw;
        fw.version = (const char*)(b["version"] | "");
        fw.commit  = (const char*)(b["commit"]  | "");
        fw.date    = (const char*)(b["date"]    | "");
        fw.file    = (const char*)(app["file"]  | "");
        fw.md5     = (const char*)(app["md5"]   | "");
        fw.size    = app["size"] | 0;
        if (fw.file.length()) out.push_back(fw);
    }
    if (out.empty()) { err = "No builds in manifest"; return false; }
    return true;
}

static bool installFromUrl(const FwBuild& b, String& err) {
    WiFiClientSecure client;
    client.setInsecure();   // see fetchManifest note; md5 is checked below
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    String url = manifestBaseUrl() + b.file;
    Serial.printf("[FWUP] Downloading %s\n", url.c_str());
    if (!http.begin(client, url)) { err = "Bad firmware URL"; return false; }
    int code = http.GET();
    if (code != 200) {
        err = "Download HTTP " + String(code);
        http.end();
        return false;
    }
    int len = http.getSize();
    size_t total = len > 0 ? (size_t)len : b.size;

    paintUpdateStart(b.file);
    Update.onProgress([](size_t done, size_t t) {
        if (t) paintUpdatePct((int)(done * 100 / t));
    });
    if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN)) {
        err = Update.errorString();
        http.end();
        return false;
    }
    if (b.md5.length() == 32) Update.setMD5(b.md5.c_str());

    size_t written = Update.writeStream(http.getStream());
    http.end();
    if (total && written != total) {
        err = "Short read " + String(written) + "/" + String(total);
        Update.abort();
        return false;
    }
    if (!Update.end(true)) {
        err = Update.errorString();
        return false;
    }
    Serial.printf("[FWUP] Flashed %u bytes from %s\n", (unsigned)written, b.file.c_str());
    return true;
}

// --- Menu drawing (DisplayUI's centred-text helper is private, so the
// ad-hoc screens below use their own via getTextBounds) ---
static void fwCenterText(const String& s, int cx, int cy, int size,
                         uint16_t fg, uint16_t bg) {
    auto* g = ui.gfx();
    ui.applyTextSize(size);
    int16_t x1, y1; uint16_t w, h;
    g->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
    g->setTextColor(fg, bg);
    g->setCursor(cx - w / 2 - x1, cy - h / 2 - y1);
    g->print(s);
}

#define FWM_HDR_H  64
#define FWM_Y0     90
#define FWM_X      20
#define FWM_W      (SCREEN_WIDTH - 40)
#define FWM_ROW_H  90
#define FWM_GAP    14
#define FWM_MAX    6

static void drawUpdateMenu(const std::vector<FwBuild>& builds, int shown) {
    auto* g = ui.gfx();
    g->fillScreen(COL_BG);
    g->fillRect(0, 0, SCREEN_WIDTH, FWM_HDR_H, COL_HEADER_BG);
    fwCenterText("Firmware", SCREEN_WIDTH / 2, FWM_HDR_H / 2, 3, COL_BG, COL_HEADER_BG);
    g->fillRoundRect(6, (FWM_HDR_H - 38) / 2, 78, 38, 7, COL_BG);
    fwCenterText("< Back", 6 + 39, FWM_HDR_H / 2, 2, COL_ACCENT, COL_BG);

    for (int i = 0; i < shown; i++) {
        const FwBuild& b = builds[i];
        int y = FWM_Y0 + i * (FWM_ROW_H + FWM_GAP);
        g->fillRoundRect(FWM_X, y, FWM_W, FWM_ROW_H, 10, COL_KEYPAD_BG);

        String line1 = "v" + b.version + "  @" + b.commit;
        if (i == 0) line1 += "  (latest)";
        else if (b.commit == FW_GIT_REV) line1 += "  (installed)";
        String line2 = b.date + "  -  " + String(b.size / 1024) + " KB";
        fwCenterText(line1, SCREEN_WIDTH / 2, y + 30, 2, COL_FG, COL_KEYPAD_BG);
        fwCenterText(line2, SCREEN_WIDTH / 2, y + 62, 2, COL_DIM, COL_KEYPAD_BG);
    }

    fwCenterText(String("Running v") + FW_VERSION + " @" + FW_GIT_REV,
                 SCREEN_WIDTH / 2, SCREEN_HEIGHT - 28, 2, COL_DIM, COL_BG);
}

// Full-screen confirm with Install / Cancel buttons. Blocks on touch.
static bool confirmInstall(const FwBuild& b) {
    auto* g = ui.gfx();
    g->fillScreen(COL_BG);
    fwCenterText("Install firmware?", SCREEN_WIDTH / 2, 160, 3, COL_ACCENT, COL_BG);
    fwCenterText("v" + b.version + "  @" + b.commit, SCREEN_WIDTH / 2, 240, 2, COL_FG, COL_BG);
    fwCenterText(b.date, SCREEN_WIDTH / 2, 280, 2, COL_DIM, COL_BG);
    fwCenterText("The POS reboots when done", SCREEN_WIDTH / 2, 330, 2, COL_DIM, COL_BG);

    const int bw = SCREEN_WIDTH - 80, bh = 76;
    const int yInstall = 420, yCancel = 530;
    g->fillRoundRect(40, yInstall, bw, bh, 12, COL_SUCCESS);
    fwCenterText("Install", SCREEN_WIDTH / 2, yInstall + bh / 2, 3, COL_BG, COL_SUCCESS);
    g->fillRoundRect(40, yCancel, bw, bh, 12, COL_KEYPAD_BG);
    fwCenterText("Cancel", SCREEN_WIDTH / 2, yCancel + bh / 2, 3, COL_FG, COL_KEYPAD_BG);

    while (true) {
        uint16_t tx, ty;
        if (ui.touchPoint(tx, ty) && tx >= 40 && tx < 40 + bw) {
            if (ty >= yInstall && ty < yInstall + bh) return true;
            if (ty >= yCancel  && ty < yCancel  + bh) return false;
        }
        delay(20);
    }
}

void FirmwarePortal::runUpdateMenu() {
    if (WiFi.status() != WL_CONNECTED) {
        ui.showError("No WiFi connection");
        delay(2000);
        return;
    }

    ui.showLoading("Checking for updates...");
    std::vector<FwBuild> builds;
    String err;
    if (!fetchManifest(builds, err)) {
        Serial.printf("[FWUP] Manifest fetch failed: %s\n", err.c_str());
        ui.showError(err);
        delay(2500);
        return;
    }
    Serial.printf("[FWUP] Manifest: %d build(s)\n", (int)builds.size());

    int shown = builds.size() < FWM_MAX ? (int)builds.size() : FWM_MAX;
    drawUpdateMenu(builds, shown);

    while (true) {
        uint16_t tx, ty;
        if (ui.touchPoint(tx, ty)) {
            // Back (top-left header corner)
            if (tx < 100 && ty < FWM_HDR_H) return;

            for (int i = 0; i < shown; i++) {
                int y = FWM_Y0 + i * (FWM_ROW_H + FWM_GAP);
                if (tx >= FWM_X && tx < FWM_X + FWM_W && ty >= y && ty < y + FWM_ROW_H) {
                    if (confirmInstall(builds[i])) {
                        String ierr;
                        if (installFromUrl(builds[i], ierr)) {
                            fwCenterText("Update complete - rebooting",
                                         SCREEN_WIDTH / 2, SCREEN_HEIGHT - 120, 2,
                                         COL_SUCCESS, COL_BG);
                            delay(1500);
                            ESP.restart();
                        }
                        Serial.printf("[FWUP] Install failed: %s\n", ierr.c_str());
                        ui.showError(ierr);
                        delay(2500);
                    }
                    drawUpdateMenu(builds, shown);
                    break;
                }
            }
        }
        delay(20);
    }
}
