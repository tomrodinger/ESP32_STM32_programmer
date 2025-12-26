#include "wifi_web_ui.h"

#include <WiFi.h>
#include <WebServer.h>

#include <SPIFFS.h>

#include "firmware_fs.h"
#include "program_state.h"
#include "serial_log.h"

#include "ram_log.h"
#include "tee_log.h"

// Route all Serial prints in this file into the RAM terminal buffer as well.
#define Serial tee_log::out()

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

static bool parse_http_range_bytes(const String &range, size_t total_len, size_t *out_start, size_t *out_len) {
  if (out_start) *out_start = 0;
  if (out_len) *out_len = total_len;
  if (range.length() == 0) return false;

  // Expected: "bytes=<start>-<end>" (end may be omitted).
  if (!range.startsWith("bytes=")) return false;
  const int dash = range.indexOf('-', 6);
  if (dash < 0) return false;

  const String a = range.substring(6, dash);
  const String b = range.substring(dash + 1);

  if (total_len == 0) return false;

  // Support:
  // - bytes=START-END
  // - bytes=START-
  // - bytes=-SUFFIX (last SUFFIX bytes)
  if (a.length() == 0) {
    // Suffix range
    if (b.length() == 0) return false;
    const size_t suffix = (size_t)b.toInt();
    if (suffix == 0) return false;
    const size_t start = (suffix >= total_len) ? 0 : (total_len - suffix);
    const size_t len = total_len - start;
    if (out_start) *out_start = start;
    if (out_len) *out_len = len;
    return true;
  }

  const size_t start = (size_t)a.toInt();
  if (start >= total_len) return false;

  size_t end_incl = total_len - 1;
  if (b.length() > 0) {
    end_incl = (size_t)b.toInt();
    if (end_incl >= total_len) end_incl = total_len - 1;
    if (end_incl < start) return false;
  }

  const size_t len = (end_incl - start) + 1;
  if (out_start) *out_start = start;
  if (out_len) *out_len = len;
  return true;
}

static void send_ram_log_as_text(bool include_header) {
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.send(200, "text/plain", "");

  if (include_header) {
    char hdr[176];
    snprintf(hdr, sizeof(hdr), "# ram_log: size=%lu bytes, capacity=%lu bytes, total_written=%lu\n",
             (unsigned long)ram_log::size(), (unsigned long)ram_log::capacity(),
             (unsigned long)ram_log::total_written());
    g_server.sendContent(hdr);
  }

  class SendContentPrint final : public Print {
   public:
    size_t write(uint8_t b) override {
      const char c = (char)b;
      g_server.sendContent(&c, 1);
      return 1;
    }
    size_t write(const uint8_t *buffer, size_t size) override {
      g_server.sendContent(reinterpret_cast<const char *>(buffer), size);
      return size;
    }
  } sink;

  ram_log::stream_to(sink);
}

static void send_ram_log_as_download_text(bool include_header) {
  // NOTE: for best download-compatibility we do NOT include the textual header
  // in the download response; it breaks range semantics and some download
  // managers will retry/abort.
  (void)include_header;

  // IMPORTANT: to keep Content-Length consistent, disable RAM-log capture while
  // serving the download.
  tee_log::ScopedCaptureSuspend suspend;

  const size_t total = ram_log::size();

  // Some download managers issue HEAD and/or Range GETs.
  size_t start = 0;
  size_t len = total;
  const String range = g_server.header("Range");
  const bool has_range_hdr = (range.length() > 0);
  const bool is_range = parse_http_range_bytes(range, total, &start, &len);

  g_server.sendHeader("Cache-Control", "no-store");
  g_server.sendHeader("Connection", "close");
  g_server.sendHeader("Accept-Ranges", "bytes");
  g_server.sendHeader("X-RamLog-Size", String((unsigned long)total));
  g_server.sendHeader("X-RamLog-Capacity", String((unsigned long)ram_log::capacity()));
  g_server.sendHeader("X-RamLog-TotalWritten", String((unsigned long)ram_log::total_written()));

  if (has_range_hdr && !is_range) {
    // Range requested but we can't satisfy it.
    char cr[64];
    snprintf(cr, sizeof(cr), "bytes */%lu", (unsigned long)total);
    g_server.sendHeader("Content-Range", cr);
    g_server.send(416, "application/octet-stream", "");
    return;
  }

  if (is_range) {
    const size_t end_incl = start + len - 1;
    char cr[96];
    snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu", (unsigned long)start, (unsigned long)end_incl, (unsigned long)total);
    g_server.sendHeader("Content-Range", cr);
    g_server.setContentLength(len);
    g_server.send(206, "application/octet-stream", "");
  } else {
    g_server.setContentLength(total);
    g_server.send(200, "application/octet-stream", "");
  }

  WiFiClient client = g_server.client();
  client.setNoDelay(true);

  const uint8_t *p1 = nullptr;
  const uint8_t *p2 = nullptr;
  size_t n1 = 0;
  size_t n2 = 0;
  ram_log::snapshot_spans(&p1, &n1, &p2, &n2);

  // Skip `start` bytes into the spans.
  size_t skip = start;
  if (skip > 0 && n1 > 0) {
    const size_t s = (skip < n1) ? skip : n1;
    p1 += s;
    n1 -= s;
    skip -= s;
  }
  if (skip > 0 && n2 > 0) {
    const size_t s = (skip < n2) ? skip : n2;
    p2 += s;
    n2 -= s;
    skip -= s;
  }

  // Use MTU-friendly chunk size (1460 is typical for Ethernet/WiFi minus headers).
  auto write_all = [&](const uint8_t *p, size_t n) -> bool {
    if (!p || n == 0) return true;
    size_t off = 0;
    uint32_t zero_writes = 0;
    while (off < n) {
      const size_t want = ((n - off) > 1460u) ? 1460u : (n - off);
      const size_t w = client.write(p + off, want);
      if (w == 0) {
        zero_writes++;
        delay(2);
        if (!client.connected() || zero_writes > 8) return false;
        continue;
      }
      off += w;
      zero_writes = 0;
      client.flush();  // Flush after each chunk for download manager compatibility.
      delay(1);        // Yield to WiFi/LwIP stack.
    }
    return true;
  };

  size_t remain = len;
  if (p1 && n1 > 0 && remain > 0) {
    const size_t take1 = (n1 < remain) ? n1 : remain;
    if (!write_all(p1, take1)) {
      client.stop();
      return;
    }
    remain -= take1;
  }
  if (p2 && n2 > 0 && remain > 0) {
    const size_t take2 = (n2 < remain) ? n2 : remain;
    if (!write_all(p2, take2)) {
      client.stop();
      return;
    }
    remain -= take2;
  }

  client.flush();
  client.stop();  // Actively close the connection for download manager compatibility.
}

static void send_mem_json() {
  // "Easy to attain" values without linker/map parsing. Intended for leak/
  // fragmentation monitoring.
  String json = "{";
  json += "\"heap_size\":" + String((unsigned long)ESP.getHeapSize());
  json += ",\"free_heap\":" + String((unsigned long)ESP.getFreeHeap());
  json += ",\"min_free_heap\":" + String((unsigned long)ESP.getMinFreeHeap());
  json += ",\"max_alloc_heap\":" + String((unsigned long)ESP.getMaxAllocHeap());
#if defined(BOARD_HAS_PSRAM)
  json += ",\"psram_size\":" + String((unsigned long)ESP.getPsramSize());
  json += ",\"free_psram\":" + String((unsigned long)ESP.getFreePsram());
#endif
  json += "}";
  g_server.send(200, "application/json", json);
}

static void stream_consumed_records_as_text(File &f, bool include_indices, bool annotate_marker, bool header_comment) {
  // Stream conversion from binary LE u32 to text, to avoid allocating large Strings.
  // Uses chunked transfer (CONTENT_LENGTH_UNKNOWN).

  const size_t sz = (size_t)f.size();
  if ((sz % 4u) != 0u) {
    g_server.sendContent("ERROR: corrupt consumed record (size not multiple of 4)\n");
    return;
  }

  if (header_comment) {
    g_server.sendContent("# serial_consumed.bin decoded as little-endian uint32 entries\n");
    if (annotate_marker) {
      g_server.sendContent("# NOTE: value 0 indicates USERSET marker; next entry is the user-set next-serial seed\n");
    }
  }

  const uint32_t total = (uint32_t)(sz / 4u);
  uint8_t b[4];
  for (uint32_t idx = 0; idx < total; idx++) {
    const int r = f.read(b, sizeof(b));
    if (r != (int)sizeof(b)) {
      g_server.sendContent("ERROR: short read\n");
      return;
    }
    const uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

    char line[128];
    if (include_indices) {
      if (annotate_marker && v == 0x00000000u) {
        snprintf(line, sizeof(line), "[%lu] 0 (USERSET marker; next entry is next-serial seed)\n", (unsigned long)idx);
      } else {
        snprintf(line, sizeof(line), "[%lu] %lu\n", (unsigned long)idx, (unsigned long)v);
      }
    } else {
      // Plain list: one number per line.
      snprintf(line, sizeof(line), "%lu\n", (unsigned long)v);
    }
    g_server.sendContent(line);
  }
}

static const char k_index_html[] PROGMEM =
    "<!doctype html>\n"
    "<html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
    "<title>Gearotons M17 Programming and Testing Jig</title>"
    "<style>body{font-family:system-ui;margin:16px}code{background:#f3f3f3;padding:2px 4px}</style>"
    "</head><body>"
    "<h2>Gearotons M17 Programming and Testing Jig</h2>"
    "<p>Firmware file (active): <code id='fw'>...</code></p>"
    "<p>Next serial: <code id='sn'>...</code></p>"
    "<p>Filesystem free: <code id='fsfree'>...</code> bytes (est. <code id='unitsleft'>...</code> units left)</p>"
    "<p>Memory: <code id='memline'>...</code> <button onclick='refreshMem()'>Refresh</button></p>"
    "<p>Programming enabled: <code id='progok'>...</code></p>"
    "<hr/>"
    "<h3>Firmware management</h3>"
    "<div style='display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start'>"
    "  <div style='flex:1;min-width:320px'>"
    "    <div style='margin-bottom:6px'>Stored BL* files:</div>"
    "    <div id='fwfiles' style='border:1px solid #ccc;padding:8px'></div>"
    "  </div>"
    "  <div style='flex:1;min-width:320px'>"
    "    <div style='margin-bottom:6px'>Upload new firmware (.bin):</div>"
    "    <input id='fwup' type='file' accept='.bin,application/octet-stream'/>"
    "    <button onclick='uploadFw()' style='margin-left:8px'>Upload</button>"
    "    <div id='fwupmsg' style='margin-top:6px;white-space:pre-wrap;font-family:ui-monospace,Menlo,monospace'></div>"
    "  </div>"
    "</div>"
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
    "  <button onclick='viewLogs()'>View Logs</button>"
    "  <button onclick=\"window.location='/download/log.txt'\">Download log.txt</button>"
    "  <button onclick=\"window.location='/download/serial_consumed.bin'\">Download consumed serials</button>"
    "</div>"
    "<div style='display:flex;gap:12px;flex-wrap:wrap;margin-top:12px'>"
    "  <div style='flex:1;min-width:320px'>"
    "    <div>Consumed serial records:</div>"
    "    <div id='consumedbox' style='margin-top:6px;white-space:pre;font-family:ui-monospace,Menlo,monospace;"
    "border:1px solid #ccc;padding:8px;max-height:280px;overflow:auto'></div>"
    "  </div>"
    "  <div style='flex:1;min-width:320px'>"
    "    <div>log.txt:</div>"
    "    <div id='logbox' style='margin-top:6px;white-space:pre;font-family:ui-monospace,Menlo,monospace;"
    "border:1px solid #ccc;padding:8px;max-height:280px;overflow:auto'></div>"
    "  </div>"
    "</div>"
    "<div style='margin-top:12px'>"
    "  <button onclick='viewRamLog()'>View RAM Terminal Buffer</button>"
    "  <button onclick=\"window.location='/download/ram_log.txt'\">Download RAM Terminal Buffer</button>"
    "</div>"
    "<div id='ramlogbox' style='margin-top:6px;white-space:pre;font-family:ui-monospace,Menlo,monospace;'"
    "background:#f7f7f7;border:1px solid #ddd;padding:8px;max-height:280px;overflow:auto'></div>"
    "<script>\n"
    "async function refresh(){\n"
    "  const r=await fetch('/api/status');\n"
    "  const j=await r.json();\n"
    "  document.getElementById('fw').textContent=j.firmware_filename||'';\n"
    "  document.getElementById('sn').textContent=String(j.serial_next||0);\n"
    "  document.getElementById('fsfree').textContent=String(j.fs_free_bytes||0);\n"
    "  document.getElementById('unitsleft').textContent=String(j.units_remaining_estimate||0);\n"
    "  document.getElementById('progok').textContent=(j.fs_ok? 'YES':'NO (select firmware)');\n"
    "  document.getElementById('statusjson').textContent=JSON.stringify(j);\n"
    "  await refreshFwList();\n"
    "}\n"
    "async function refreshFwList(){\n"
    "  const r=await fetch('/api/firmware/list');\n"
    "  const j=await r.json();\n"
    "  const box=document.getElementById('fwfiles');\n"
    "  const files=j.files||[];\n"
    "  const active=j.active||'';\n"
    "  if(files.length===0){box.textContent='(none)';return;}\n"
    "  let h='';\n"
    "  for(const f of files){\n"
    "    const checked=(f===active)?'checked':'';\n"
    "    h += '<div style=\"display:flex;gap:8px;align-items:center;margin:4px 0\">' +\n"
    "         '<input type=\"radio\" name=\"fwsel\" value=\"' + f + '\" ' + checked + ' onchange=\"selectFw(this.value)\"/>' +\n"
    "         '<code style=\"flex:1\">' + f + '</code>' +\n"
    "         '<button data-name=\"' + f + '\" onclick=\"deleteFw(this.dataset.name)\">Delete</button>' +\n"
    "         '</div>';\n"
    "  }\n"
    "  box.innerHTML=h;\n"
    "}\n"
    "async function selectFw(name){\n"
    "  const r=await fetch('/api/firmware/select',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({basename:name})});\n"
    "  const t=await r.text();\n"
    "  document.getElementById('statusjson').textContent=t;\n"
    "  refresh();\n"
    "}\n"
    "async function deleteFw(name){\n"
    "  if(!confirm('Delete firmware file '+name+' ?')) return;\n"
    "  const r=await fetch('/api/firmware/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({basename:name})});\n"
    "  const t=await r.text();\n"
    "  document.getElementById('statusjson').textContent=t;\n"
    "  refresh();\n"
    "}\n"
    "async function uploadFw(){\n"
    "  const inp=document.getElementById('fwup');\n"
    "  if(!inp.files||inp.files.length===0){document.getElementById('fwupmsg').textContent='No file selected';return;}\n"
    "  const f=inp.files[0];\n"
    "  const fd=new FormData();\n"
    "  fd.append('fw',f,f.name);\n"
    "  const r=await fetch('/api/firmware/upload',{method:'POST',body:fd});\n"
    "  const t=await r.text();\n"
    "  document.getElementById('fwupmsg').textContent=t;\n"
    "  refresh();\n"
    "}\n"
    "async function setSerial(){\n"
    "  const v=document.getElementById('setv').value;\n"
    "  const r=await fetch('/api/serial',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({serial_next:Number(v)})});\n"
    "  const t=await r.text();\n"
    "  document.getElementById('statusjson').textContent=t;\n"
    "  try{const j=JSON.parse(t);document.getElementById('sn').textContent=String(j.serial_next||0);}catch(e){}\n"
    "}\n"
    "async function viewLogs(){\n"
    "  const r1=await fetch('/api/consumed');\n"
    "  const t1=await r1.text();\n"
    "  document.getElementById('consumedbox').textContent=t1;\n"
    "  const r2=await fetch('/api/log');\n"
    "  const t2=await r2.text();\n"
    "  document.getElementById('logbox').textContent=t2;\n"
    "}\n"
    "async function refreshMem(){\n"
    "  const r=await fetch('/api/mem');\n"
    "  const j=await r.json();\n"
    "  const s='heap free '+j.free_heap+' / '+j.heap_size+' (min '+j.min_free_heap+', max_alloc '+j.max_alloc_heap+')';\n"
    "  document.getElementById('memline').textContent=s;\n"
    "}\n"
    "async function viewRamLog(){\n"
    "  const r=await fetch('/api/ram_log');\n"
    "  const t=await r.text();\n"
    "  document.getElementById('ramlogbox').textContent=t;\n"
    "}\n"
    "refresh();refreshMem();setInterval(refresh,3000);\n"
    "</script></body></html>\n";

static void send_status_json() {
  const String fw_path = program_state::firmware_filename();
  const uint32_t sn = serial_log::serial_next();

  const size_t fs_total = (size_t)SPIFFS.totalBytes();
  const size_t fs_used = (size_t)SPIFFS.usedBytes();
  const size_t fs_free = (fs_total > fs_used) ? (fs_total - fs_used) : 0u;
  const bool fw_ok = fw_path.length() > 0;
  const bool fs_ok = (fs_free >= 100u) && fw_ok;

  const uint32_t bytes_per_unit = serial_log::bytes_per_unit_estimate();
  const uint32_t units_remaining = (bytes_per_unit > 0u) ? (uint32_t)(fs_free / (size_t)bytes_per_unit) : 0u;

  // Status JSON response.
  String json = "{";
  json += "\"firmware_filename\":";
  if (fw_path.length() > 0) {
    json += "\"" + fw_path + "\"";
  } else {
    json += "\"\"";
  }
  json += ",\"serial_next\":" + String((unsigned long)sn);
  json += ",\"fs_total_bytes\":" + String((unsigned long)fs_total);
  json += ",\"fs_used_bytes\":" + String((unsigned long)fs_used);
  json += ",\"fs_free_bytes\":" + String((unsigned long)fs_free);
  json += ",\"fs_ok\":" + String(fs_ok ? "true" : "false");
  json += ",\"firmware_selected\":" + String(fw_ok ? "true" : "false");
  json += ",\"bytes_per_unit_estimate\":" + String((unsigned long)bytes_per_unit);
  json += ",\"units_remaining_estimate\":" + String((unsigned long)units_remaining);
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

static bool parse_json_string_field(const String &body, const char *field, String &out_value) {
  out_value = "";
  if (!field) return false;

  const int key = body.indexOf(field);
  if (key < 0) return false;
  const int colon = body.indexOf(':', key);
  if (colon < 0) return false;

  // Find the first quote after the colon.
  const int q1 = body.indexOf('"', colon);
  if (q1 < 0) return false;
  const int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  out_value = body.substring(q1 + 1, q2);
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
  // Allow access to selected request headers (Arduino WebServer does not expose
  // arbitrary headers unless explicitly collected).
  static const char *k_header_keys[] = {"Range"};
  g_server.collectHeaders(k_header_keys, 1);

  g_server.on("/", HTTP_GET, []() {
    g_server.send(200, "text/html", FPSTR(k_index_html));
  });
  g_server.on("/api/status", HTTP_GET, []() { send_status_json(); });
  g_server.on("/api/mem", HTTP_GET, []() { send_mem_json(); });
  g_server.on("/api/serial", HTTP_POST, []() { handle_post_serial(); });

  g_server.on("/api/firmware/list", HTTP_GET, []() {
    String active_path;
    bool auto_sel = false;
    (void)firmware_fs::reconcile_active_selection_ex(&active_path, &auto_sel);
    String active_base = active_path;
    if (active_base.startsWith("/")) active_base = active_base.substring(1);

    if (auto_sel && active_base.length() > 0) {
      (void)serial_log::append_event("AUTOSELECT", active_base.c_str());
    }

    // Gather up to a reasonable number for UI.
    String names[32];
    size_t count = 0;
    if (!firmware_fs::list_firmware_basenames(names, 32, &count)) {
      g_server.send(500, "application/json", "{\"error\":\"fs_list_failed\"}");
      return;
    }

    String json = "{";
    json += "\"active\":\"" + active_base + "\",";
    json += "\"files\":[";
    for (size_t i = 0; i < count && i < 32; i++) {
      if (i) json += ",";
      json += "\"" + names[i] + "\"";
    }
    json += "]}";
    g_server.send(200, "application/json", json);
  });

  g_server.on("/api/firmware/select", HTTP_POST, []() {
    const String body = g_server.arg("plain");
    String name;
    if (!parse_json_string_field(body, "basename", name)) {
      g_server.send(400, "text/plain", "Bad request: expected {basename:\"BL...\"}\n");
      return;
    }

    if (!firmware_fs::set_active_firmware_basename(name)) {
      g_server.send(400, "text/plain", "Failed to select firmware (missing/invalid?)\n");
      return;
    }

    (void)serial_log::append_event("USERSELECT", name.c_str());

    String active;
    (void)firmware_fs::reconcile_active_selection(&active);
    program_state::set_firmware_filename(active);
    send_status_json();
  });

  g_server.on("/api/firmware/delete", HTTP_POST, []() {
    const String body = g_server.arg("plain");
    String name;
    if (!parse_json_string_field(body, "basename", name)) {
      g_server.send(400, "text/plain", "Bad request: expected {basename:\"BL...\"}\n");
      return;
    }
    const String path = String("/") + name;
    if (!SPIFFS.exists(path)) {
      g_server.send(404, "text/plain", "File not found\n");
      return;
    }

    // If deleting active file, clear selection first.
    const String active_path = program_state::firmware_filename();
    if (active_path == path) {
      (void)firmware_fs::clear_active_firmware_selection();
      program_state::set_firmware_filename("");
    }

    if (!SPIFFS.remove(path)) {
      g_server.send(500, "text/plain", "Delete failed\n");
      return;
    }

    String active;
    bool auto_sel = false;
    (void)firmware_fs::reconcile_active_selection_ex(&active, &auto_sel);
    program_state::set_firmware_filename(active);

    if (auto_sel && active.startsWith("/")) {
      const String b = active.substring(1);
      if (b.length() > 0) (void)serial_log::append_event("AUTOSELECT", b.c_str());
    }
    send_status_json();
  });

  // Firmware upload via multipart/form-data.
  // Field name: "fw".
  static File upload_file;
  static String upload_target_path;
  static String upload_err;
  g_server.on(
      "/api/firmware/upload", HTTP_POST,
      []() {
        if (upload_err.length() > 0) {
          g_server.send(400, "text/plain", upload_err + "\n");
          upload_err = "";
          return;
        }
        g_server.send(200, "text/plain", "OK\n");
      },
      []() {
        HTTPUpload &up = g_server.upload();
        if (up.status == UPLOAD_FILE_START) {
          upload_err = "";
          upload_target_path = "";
          if (upload_file) {
            upload_file.close();
          }
          String base;
          String err;
          if (!firmware_fs::normalize_uploaded_firmware_filename(up.filename, base, &err)) {
            upload_err = String("ERROR: ") + err;
            return;
          }
          upload_target_path = String("/") + base;
          upload_file = SPIFFS.open(upload_target_path, "w");
          if (!upload_file) {
            upload_err = "ERROR: could not open file for write";
            return;
          }
        } else if (up.status == UPLOAD_FILE_WRITE) {
          if (upload_err.length() > 0) return;
          if (!upload_file) {
            upload_err = "ERROR: upload file not open";
            return;
          }
          const size_t w = upload_file.write(up.buf, up.currentSize);
          if (w != up.currentSize) {
            upload_err = "ERROR: short write";
            return;
          }
        } else if (up.status == UPLOAD_FILE_END) {
          if (upload_file) {
            upload_file.flush();
            upload_file.close();
          }
          if (upload_err.length() > 0) return;

          // Auto-select if needed.
          String active;
          bool auto_sel = false;
          (void)firmware_fs::reconcile_active_selection_ex(&active, &auto_sel);
          program_state::set_firmware_filename(active);

          if (auto_sel && active.startsWith("/")) {
            const String b = active.substring(1);
            if (b.length() > 0) (void)serial_log::append_event("AUTOSELECT", b.c_str());
          }
        } else if (up.status == UPLOAD_FILE_ABORTED) {
          if (upload_file) upload_file.close();
          upload_err = "ERROR: upload aborted";
        }
      });
  g_server.on("/api/logs", HTTP_GET, []() {
    // Simple combined view for in-browser viewing (full content).
    g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_server.send(200, "text/plain", "");

    g_server.sendContent("===== log.txt =====\n");
    {
      File f = SPIFFS.open(serial_log::log_path(), "r");
      if (!f) {
        g_server.sendContent("(missing)\n");
      } else {
        uint8_t buf[512];
        while (true) {
          const int r = f.read(buf, sizeof(buf));
          if (r <= 0) break;
          g_server.sendContent(reinterpret_cast<const char *>(buf), (size_t)r);
        }
        f.close();
      }
    }

    g_server.sendContent("\n\n===== serial_consumed (decoded) =====\n");
    {
      File f = SPIFFS.open(serial_log::consumed_records_path(), "r");
      if (!f) {
        g_server.sendContent("(missing)\n");
      } else {
        stream_consumed_records_as_text(f, /*include_indices=*/true, /*annotate_marker=*/true, /*header_comment=*/false);
        f.close();
      }
    }
  });
  g_server.on("/api/log", HTTP_GET, []() {
    if (!SPIFFS.exists(serial_log::log_path())) {
      g_server.send(404, "text/plain", "Log not found\n");
      return;
    }
    File f = SPIFFS.open(serial_log::log_path(), "r");
    if (!f) {
      g_server.send(404, "text/plain", "Log not found\n");
      return;
    }
    g_server.streamFile(f, "text/plain");
    f.close();
  });

  g_server.on("/api/consumed", HTTP_GET, []() {
    if (!SPIFFS.exists(serial_log::consumed_records_path())) {
      g_server.send(404, "text/plain", "Consumed serial record not found\n");
      return;
    }
    File f = SPIFFS.open(serial_log::consumed_records_path(), "r");
    if (!f) {
      g_server.send(404, "text/plain", "Consumed serial record not found\n");
      return;
    }

    g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_server.send(200, "text/plain", "");
    stream_consumed_records_as_text(f, /*include_indices=*/true, /*annotate_marker=*/true, /*header_comment=*/false);
    f.close();
  });

  // RAM terminal buffer view.
  g_server.on("/api/ram_log", HTTP_GET, []() { send_ram_log_as_text(/*include_header=*/true); });

  // Download endpoints (full files).
  g_server.on("/download/log.txt", HTTP_GET, []() {
    File f = SPIFFS.open(serial_log::log_path(), "r");
    if (!f) {
      g_server.send(404, "text/plain", "Log not found\n");
      return;
    }
    g_server.sendHeader("Content-Disposition", "attachment; filename=log.txt");
    g_server.streamFile(f, "text/plain");
    f.close();
  });

  g_server.on("/download/serial_consumed.bin", HTTP_GET, []() {
    File f = SPIFFS.open(serial_log::consumed_records_path(), "r");
    if (!f) {
      g_server.send(404, "text/plain", "Consumed serial record not found\n");
      return;
    }
    g_server.sendHeader("Content-Disposition", "attachment; filename=serial_consumed.bin");
    g_server.streamFile(f, "application/octet-stream");
    f.close();
  });

  // RAM terminal buffer download.
  g_server.on("/download/ram_log.txt", HTTP_GET, []() {
    g_server.sendHeader("Content-Disposition", "attachment; filename=ram_log.txt");
    g_server.sendHeader("Content-Transfer-Encoding", "binary");
    send_ram_log_as_download_text(/*include_header=*/false);
  });

  // Some clients issue HEAD before GET.
  g_server.on("/download/ram_log.txt", HTTP_HEAD, []() {
    tee_log::ScopedCaptureSuspend suspend;
    const size_t total = ram_log::size();

    size_t start = 0;
    size_t len = total;
    const String range = g_server.header("Range");
    const bool has_range_hdr = (range.length() > 0);
    const bool is_range = parse_http_range_bytes(range, total, &start, &len);

    g_server.sendHeader("Content-Disposition", "attachment; filename=ram_log.txt");
    g_server.sendHeader("Content-Transfer-Encoding", "binary");
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.sendHeader("Pragma", "no-cache");
    g_server.sendHeader("Connection", "close");
    g_server.sendHeader("Accept-Ranges", "bytes");

    if (has_range_hdr && !is_range) {
      char cr[64];
      snprintf(cr, sizeof(cr), "bytes */%lu", (unsigned long)total);
      g_server.sendHeader("Content-Range", cr);
      g_server.send(416, "application/octet-stream", "");
      return;
    }

    if (is_range) {
      const size_t end_incl = start + len - 1;
      char cr[96];
      snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu", (unsigned long)start, (unsigned long)end_incl, (unsigned long)total);
      g_server.sendHeader("Content-Range", cr);
      g_server.setContentLength(len);
      g_server.send(206, "application/octet-stream", "");
      return;
    }

    g_server.setContentLength(total);
    g_server.send(200, "application/octet-stream", "");
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

#undef Serial
