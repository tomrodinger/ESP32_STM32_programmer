#include "wifi_web_ui.h"

#include <WiFi.h>
#include <WebServer.h>

#include <SPIFFS.h>

#include "firmware_fs.h"
#include "program_state.h"
#include "serial_log.h"

// Secrets: prefer local non-committed header.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#elif __has_include("wifi_secrets.example.h")
#include "wifi_secrets.example.h"
#else
#define WIFI_AP_SSID "ESP32_STM32_PROG"
#define WIFI_AP_PASS "change_me_please"
#endif

namespace wifi_web_ui {

static WebServer g_server(80);
static bool g_started = false;

static const char k_index_html[] PROGMEM =
    "<!doctype html>\n"
    "<html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
    "<title>Gearotons M17 Programming and Testing Jig</title>"
    "<style>body{font-family:system-ui;margin:16px}code{background:#f3f3f3;padding:2px 4px}</style>"
    "</head><body>"
    "<h2>Gearotons M17 Programming and Testing Jig</h2>"
    "<p>Firmware file: <code id='fw'>...</code></p>"
    "<p>Next serial: <code id='sn'>...</code></p>"
    "<div style='margin-top:12px'>"
    "  <input id='setv' type='number' min='0' step='1' style='width:220px' placeholder='Set next serial'/>"
    "  <button onclick='setSerial()'>Set</button>"
    "</div>"
    "<div style='margin-top:12px'>"
    "  <div>Current status:</div>"
    "  <pre id='statusjson' style='margin:6px 0 0 0;white-space:pre-wrap;font-family:ui-monospace,Menlo,monospace;"
    "background:#f7f7f7;border:1px solid #ddd;padding:8px'></pre>"
    "</div>"
    "<div style='margin-top:12px'>"
    "  <button onclick='downloadLog()'>Download Log</button>"
    "</div>"
    "<div id='logbox' style='margin-top:12px;white-space:pre;font-family:ui-monospace,Menlo,monospace;"
    "border:1px solid #ccc;padding:8px;max-height:280px;overflow:auto'></div>"
    "<script>\n"
    "async function refresh(){\n"
    "  const r=await fetch('/api/status');\n"
    "  const j=await r.json();\n"
    "  document.getElementById('fw').textContent=j.firmware_filename||'';\n"
    "  document.getElementById('sn').textContent=String(j.serial_next||0);\n"
    "  document.getElementById('statusjson').textContent=JSON.stringify({firmware_filename:(j.firmware_filename||''),serial_next:(j.serial_next||0)});\n"
    "}\n"
    "async function setSerial(){\n"
    "  const v=document.getElementById('setv').value;\n"
    "  const r=await fetch('/api/serial',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({serial_next:Number(v)})});\n"
    "  const t=await r.text();\n"
    "  document.getElementById('statusjson').textContent=t;\n"
    "  try{const j=JSON.parse(t);document.getElementById('sn').textContent=String(j.serial_next||0);}catch(e){}\n"
    "}\n"
    "async function downloadLog(){\n"
    "  const r=await fetch('/api/log');\n"
    "  const t=await r.text();\n"
    "  document.getElementById('logbox').textContent=t;\n"
    "}\n"
    "refresh();setInterval(refresh,3000);\n"
    "</script></body></html>\n";

static void send_status_json() {
  const String fw_path = program_state::firmware_filename();
  const uint32_t sn = serial_log::serial_next();

  // Minimal JSON response as planned: { firmware_filename, serial_next }
  String json = "{";
  json += "\"firmware_filename\":";
  if (fw_path.length() > 0) {
    json += "\"" + fw_path + "\"";
  } else {
    json += "\"\"";
  }
  json += ",\"serial_next\":" + String((unsigned long)sn);
  json += "}";

  g_server.send(200, "application/json", json);
}

static bool parse_serial_next_from_body(const String &body, uint32_t *out) {
  if (!out) return false;
  // Very small parser: extract first run of digits.
  int i = 0;
  while (i < (int)body.length() && !isDigit(body[i])) i++;
  if (i >= (int)body.length()) return false;
  uint64_t v = 0;
  while (i < (int)body.length() && isDigit(body[i])) {
    v = v * 10u + (uint32_t)(body[i] - '0');
    if (v > 0xFFFFFFFFull) return false;
    i++;
  }
  *out = (uint32_t)v;
  return true;
}

static void handle_post_serial() {
  const String body = g_server.arg("plain");
  uint32_t next = 0;
  if (!parse_serial_next_from_body(body, &next)) {
    g_server.send(400, "text/plain", "Bad request: expected {serial_next:<uint32>}\n");
    return;
  }
  if (!serial_log::user_set_serial_next(next)) {
    g_server.send(500, "text/plain", "Failed to persist serial\n");
    return;
  }
  send_status_json();
}

static void setup_routes() {
  g_server.on("/", HTTP_GET, []() {
    g_server.send(200, "text/html", FPSTR(k_index_html));
  });
  g_server.on("/api/status", HTTP_GET, []() { send_status_json(); });
  g_server.on("/api/serial", HTTP_POST, []() { handle_post_serial(); });
  g_server.on("/api/log", HTTP_GET, []() {
    File f = SPIFFS.open(serial_log::log_path(), "r");
    if (!f) {
      g_server.send(404, "text/plain", "Log not found\n");
      return;
    }
    String out;
    out.reserve((size_t)f.size() + 16);
    while (f.available()) {
      const int c = f.read();
      if (c < 0) break;
      out += (char)c;
    }
    f.close();
    g_server.send(200, "text/plain", out);
  });
  g_server.onNotFound([]() { g_server.send(404, "text/plain", "Not found\n"); });
}

static void wifi_task(void *) {
  // Pinning: run WiFi + web on other core to avoid impacting factory loop.
  // Note: Arduino loop typically runs on core 1 for ESP32.
  if (g_started) {
    vTaskDelete(nullptr);
    return;
  }
  g_started = true;

  WiFi.mode(WIFI_MODE_AP);
  const bool ok = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  Serial.printf("WiFi AP: %s (%s)\n", ok ? "STARTED" : "FAILED", WIFI_AP_SSID);
  Serial.printf("WiFi AP IP: %s\n", WiFi.softAPIP().toString().c_str());

  setup_routes();
  g_server.begin();
  Serial.println("HTTP server started on port 80");

  while (true) {
    g_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void start_task() {
  // If already started, no-op.
  if (g_started) return;

  // Start on core 0 ("other" core) as the plan requests.
  xTaskCreatePinnedToCore(wifi_task, "wifi_web_ui", 8192, nullptr, 1, nullptr, 0);
}

}  // namespace wifi_web_ui
