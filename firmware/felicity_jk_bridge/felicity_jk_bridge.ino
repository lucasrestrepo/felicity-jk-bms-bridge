/*
 * Felicity <-> JK BMS Pylon RS485 address-translating bridge + web dashboard
 * =========================================================================
 * Felicity Solar IVEM5048-LV (and similar)  <->  JK-PB1A16S10P (and similar)
 *
 * What it does
 *   1. Address translation. The Felicity inverter polls Pylontech-style
 *      batteries at addresses 0x12, 0x22, 0x32 ... 0x92. The JK BMS has only
 *      4 DIP switches, so it can only be addressed 0x00..0x0F and never
 *      answers. The bridge receives the inverter request, forwards it to the
 *      BMS with ADR=0x02, then returns the answer to the inverter with the
 *      original address and a recomputed checksum.
 *   2. Monitoring. It decodes the CID2=0x61 (analog data) answer the BMS is
 *      already sending to the inverter and serves it over WiFi (dashboard,
 *      JSON API, OTA update). It never queries the BMS on its own, so it adds
 *      zero extra bus traffic.
 *   3. Cache. If the BMS misses a poll, the last good answer is replayed for
 *      up to CACHE_TTL ms so the inverter does not raise a communication fault.
 *
 * Hardware: ESP32-S3 + 2x MAX485 modules powered at 3.3 V
 *   MAX485 #1 (INVERTER)         MAX485 #2 (BMS)
 *     RO -> GPIO16  DI -> GPIO17   RO -> GPIO18  DI -> GPIO19
 *     DE+RE -> GPIO4               DE+RE -> GPIO5
 *     A -> RJ45 pin 6, B -> pin 5  A -> RJ45 pin 2, B -> pin 1   (see docs/wiring.md)
 *     VCC -> 3V3, GND -> GND       VCC -> 3V3, GND -> GND
 *
 * HTTP routes:  /  dashboard   /api  JSON   /log  event log   /raw  raw payloads
 *               /update-ui  browser firmware upload (OTA)
 *
 * Arduino IDE: Board "ESP32S3 Dev Module". See README.md.
 * License: MIT
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>

// ======== CONFIGURATION ========
// Put your credentials in secrets.h (copy secrets.h.example). Never commit it.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID "your-wifi-name"
  #define WIFI_PASS "your-wifi-password"
  #define OTA_HOST  "bms-bridge"
  #define OTA_PASS  "change-me"
#endif
// ===============================

static const int INV_RX = 16, INV_TX = 17, INV_DE = 4;   // inverter side
static const int BMS_RX = 18, BMS_TX = 19, BMS_DE = 5;   // BMS side

static const char     SOI = 0x7E, EOI = 0x0D;            // frame start / end
static const uint8_t  BMS_ADDR = 0x02;                   // address the BMS answers to
static const uint32_t BAUD = 9600;
static const uint32_t BMS_TIMEOUT = 300;                 // ms to wait for the BMS
static const uint32_t FRAME_GAP = 5;                     // ms between frames
static const uint32_t CACHE_TTL = 30000;                 // ms a cached answer stays valid

static const uint8_t LED_BRIGHTNESS = 12;                // 0..255
void led(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_BUILTIN, (r * LED_BRIGHTNESS) / 255,
                             (g * LED_BRIGHTNESS) / 255,
                             (b * LED_BRIGHTNESS) / 255);
}

HardwareSerial SerialInv(1);
HardwareSerial SerialBms(2);
WebServer server(80);

// ---------- State ----------

struct Battery {
  // decoded from CID2=0x61 (what the inverter sees)
  float    voltage = 0, current = 0;                 // V, A (positive = charging)
  uint8_t  soc = 0, soh = 0;                         // %
  uint16_t cycles = 0;
  float    cellMax = 0, cellMin = 0;                 // V
  uint8_t  cellMaxNo = 0, cellMinNo = 0;
  float    tempAvg = 0, tempMax = 0, tempMin = 0, tempMos = 0;   // deg C
  uint8_t  tempMaxNo = 0, tempMinNo = 0;
  uint32_t ts = 0;                                   // millis() of last update
  bool     valid = false;

  String raw61 = "", raw62 = "", raw63 = "";         // last raw INFO payloads
} bat;

struct Stats {
  uint32_t reqRx = 0, reqBad = 0, bmsOk = 0, bmsTimeout = 0, bmsBad = 0;
  uint32_t cacheHits = 0, noAnswer = 0, txInv = 0;
} st;

uint32_t bootMs = 0, lastReqMs = 0;
String   lastBody = "";            // last good BMS answer without ADR (for the cache)
uint32_t lastBodyMs = 0;

static const int LOG_LINES = 40;
String logBuf[LOG_LINES]; int logIdx = 0;
void addLog(const String& s) {
  uint32_t t = (millis() - bootMs) / 1000; char h[16];
  snprintf(h, sizeof(h), "%02lu:%02lu:%02lu ", t / 3600, (t / 60) % 60, t % 60);
  logBuf[logIdx % LOG_LINES] = String(h) + s; logIdx++;
}

// ---------- Protocol (Pylontech low-voltage RS485, ASCII) ----------
// Frame: ~ VER(2) ADR(2) CID1(2) CID2(2) LENGTH(4) INFO(n) CHKSUM(4) \r

uint16_t pylonChecksum(const String& c) {
  uint32_t s = 0; for (size_t i = 0; i < c.length(); i++) s += (uint8_t)c[i];
  return (uint16_t)((~s + 1) & 0xFFFF);
}
// LENGTH field: 12-bit length + 4-bit checksum nibble (not recomputed here:
// INFO is passed through untouched, so the original LENGTH stays valid)
uint8_t lenChecksum(uint16_t l) {
  uint8_t n = ((l >> 8) & 0xF) + ((l >> 4) & 0xF) + (l & 0xF);
  return (uint8_t)((~n + 1) & 0x0F);
}
String hex2(uint8_t v)  { char b[3]; snprintf(b, 3, "%02X", v); return String(b); }
String hex4(uint16_t v) { char b[5]; snprintf(b, 5, "%04X", v); return String(b); }

// Add SOI, checksum and EOI to a frame body
String seal(const String& body) { return String(SOI) + body + hex4(pylonChecksum(body)) + String(EOI); }

// Check SOI and checksum of a complete frame
bool isValid(const String& t) {
  if (t.length() < 17 || t[0] != SOI) return false;
  int p = t.length() - 5;
  return hex4(pylonChecksum(t.substring(1, p))) == t.substring(p, p + 4);
}

// Read big-endian values from the hex INFO string (o = byte offset)
uint16_t w16(const String& h, int o) {
  if ((int)h.length() < (o + 2) * 2) return 0;
  return (uint16_t)strtol(h.substring(o * 2, o * 2 + 4).c_str(), nullptr, 16);
}
int16_t s16(const String& h, int o) { return (int16_t)w16(h, o); }
uint8_t b8(const String& h, int o) {
  if ((int)h.length() < (o + 1) * 2) return 0;
  return (uint8_t)strtol(h.substring(o * 2, o * 2 + 2).c_str(), nullptr, 16);
}

// ---------- Decode CID2=0x61 (analog data, 49 bytes = 98 hex chars) ----------
void decode61(const String& i) {
  if (i.length() < 98) return;
  bat.voltage = w16(i, 0) / 1000.0f;                // pack mV
  bat.current = s16(i, 2) / 100.0f;                 // signed, 0.01 A
  bat.soc     = b8(i, 4);
  bat.cycles  = w16(i, 5);
  bat.soh     = b8(i, 9);
  bat.cellMax = w16(i, 11) / 1000.0f;  bat.cellMaxNo = b8(i, 14);
  bat.cellMin = w16(i, 15) / 1000.0f;  bat.cellMinNo = b8(i, 18);
  bat.tempAvg = w16(i, 19) / 10.0f - 273.15f;       // 0.1 K
  bat.tempMax = w16(i, 21) / 10.0f - 273.15f;  bat.tempMaxNo = b8(i, 24);
  bat.tempMin = w16(i, 25) / 10.0f - 273.15f;  bat.tempMinNo = b8(i, 28);
  bat.tempMos = w16(i, 29) / 10.0f - 273.15f;
  bat.ts = millis(); bat.valid = true; bat.raw61 = i;
}

// ---------- RS485 I/O ----------
void sendFrame(HardwareSerial& s, int de, const String& t) {
  digitalWrite(de, HIGH); delayMicroseconds(100);   // driver enable
  s.print(t); s.flush();
  delayMicroseconds(100); digitalWrite(de, LOW);    // back to receive
}

// Read one frame (SOI..EOI). Keeps the web server and OTA alive while waiting.
String readFrame(HardwareSerial& s, uint32_t timeoutMs) {
  String b = ""; uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (s.available()) {
      char c = (char)s.read();
      if (c == SOI) { b = ""; b += c; }
      else if (b.length()) { b += c; if (c == EOI) return b; if (b.length() > 600) b = ""; }
    }
    server.handleClient(); ArduinoOTA.handle(); delay(1);
  }
  return "";
}

// ---------- Web dashboard ----------
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Battery Bridge</title><style>
:root{--bg:#0d1117;--fg:#e6edf3;--dim:#8b949e;--card:#161b22;--line:#30363d;
--ok:#3fb950;--warn:#d29922;--bad:#f85149;--hi:#58a6ff;
box-sizing:border-box;padding-top:env(safe-area-inset-top,0);padding-bottom:env(safe-area-inset-bottom,0)}
*{box-sizing:border-box}
body{margin:0 auto;background:var(--bg);color:var(--fg);padding:16px;max-width:820px;
font:15px/1.5 -apple-system,system-ui,sans-serif}
h1{font-size:19px;margin:0 0 2px}
h2{font-size:14px;margin:22px 0 8px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px}
.sub{color:var(--dim);font-size:13px;margin-bottom:14px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:10px}
.c{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
.c .k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.4px}
.c .v{font-size:23px;font-weight:600;margin-top:2px}
.c .u{font-size:13px;color:var(--dim);font-weight:400}
.big{grid-column:1/-1;text-align:center;padding:6px 12px 14px}
table{width:100%;border-collapse:collapse;font-size:14px}
td{padding:6px 0;border-bottom:1px solid var(--line)}
td:last-child{text-align:right;font-variant-numeric:tabular-nums}
pre{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px;
overflow-x:auto;font-size:11px;color:var(--dim);white-space:pre-wrap;word-break:break-all}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
a{color:var(--hi);text-decoration:none;margin-right:14px;font-size:13px}
</style></head><body>
<h1>Battery Bridge</h1><div class="sub" id="upd">loading...</div>
<div class="grid">
 <div class="c big">
  <svg viewBox="0 0 220 132" style="width:100%;max-width:270px;height:auto">
   <path d="M20 118 A 90 90 0 0 1 200 118" fill="none" stroke="#30363d" stroke-width="16" stroke-linecap="round"/>
   <path id="arc" d="M20 118 A 90 90 0 0 1 200 118" fill="none" stroke="var(--ok)" stroke-width="16"
     stroke-linecap="round" stroke-dasharray="283" stroke-dashoffset="283"
     style="transition:stroke-dashoffset .6s,stroke .4s"/>
   <text id="gs" x="110" y="104" text-anchor="middle" fill="var(--fg)"
     style="font:700 44px -apple-system,system-ui,sans-serif">--</text>
   <text x="110" y="126" text-anchor="middle" fill="var(--dim)"
     style="font:12px -apple-system,system-ui,sans-serif">STATE OF CHARGE</text>
  </svg>
  <div id="flow" style="font-size:15px">--</div>
 </div>
 <div class="c"><div class="k">Voltage</div><div class="v" id="v">--<span class="u"> V</span></div></div>
 <div class="c"><div class="k">Current</div><div class="v" id="i">--<span class="u"> A</span></div><div class="k" id="ik"></div></div>
 <div class="c"><div class="k">Power</div><div class="v" id="p">--<span class="u"> W</span></div><div class="k" id="pk"></div></div>
 <div class="c"><div class="k">Max cell</div><div class="v" id="cx">--<span class="u"> V</span></div><div class="k" id="cxn"></div></div>
 <div class="c"><div class="k">Min cell</div><div class="v" id="cn">--<span class="u"> V</span></div><div class="k" id="cnn"></div></div>
 <div class="c"><div class="k">Cell delta</div><div class="v" id="d">--<span class="u"> mV</span></div></div>
 <div class="c"><div class="k">MOSFET temp</div><div class="v" id="t">--<span class="u"> C</span></div></div>
</div>
<h2>Temperatures</h2>
<table id="temps"></table>
<h2>Battery</h2>
<table id="binfo"></table>
<h2>Bridge status</h2>
<table id="st"></table>
<h2>Raw payloads</h2>
<pre id="raw"></pre>
<p><a href="/log">log</a><a href="/api">json</a><a href="/raw">raw</a><a href="/update-ui">firmware update</a></p>
<script>
function f(x,n){return x==null?'--':Number(x).toFixed(n)}
async function tick(){
 try{
  const j=await(await fetch('/api')).json(), b=j.battery;
  document.getElementById('gs').textContent=b.soc+'%';
  const arc=document.getElementById('arc');
  arc.style.strokeDashoffset=283-283*b.soc/100;
  arc.style.stroke=b.soc>40?'var(--ok)':b.soc>20?'var(--warn)':'var(--bad)';

  const chg=b.current>0.1, dis=b.current<-0.1;
  const state=chg?'charging':dis?'discharging':'idle', cl=chg?'ok':dis?'warn':'';
  const fl=document.getElementById('flow');
  fl.textContent=(chg?'▲ ':dis?'▼ ':'')+state+' · '+f(Math.abs(b.power),0)+' W';
  fl.className=cl;

  document.getElementById('v').innerHTML=f(b.voltage,2)+'<span class="u"> V</span>';
  const ic=document.getElementById('i');
  ic.innerHTML=f(Math.abs(b.current),2)+'<span class="u"> A</span>';
  ic.className='v '+cl; document.getElementById('ik').textContent=state;
  const pc=document.getElementById('p');
  pc.innerHTML=f(Math.abs(b.power),0)+'<span class="u"> W</span>';
  pc.className='v '+cl; document.getElementById('pk').textContent=state;

  document.getElementById('cx').innerHTML=f(b.cellMax,3)+'<span class="u"> V</span>';
  document.getElementById('cxn').textContent='cell '+b.cellMaxNo;
  document.getElementById('cn').innerHTML=f(b.cellMin,3)+'<span class="u"> V</span>';
  document.getElementById('cnn').textContent='cell '+b.cellMinNo;
  const de=document.getElementById('d');
  de.innerHTML=f(b.delta,0)+'<span class="u"> mV</span>';
  de.className='v '+(b.delta<30?'ok':b.delta<80?'warn':'bad');
  document.getElementById('t').innerHTML=f(b.tempMos,1)+'<span class="u"> C</span>';

  document.getElementById('temps').innerHTML=
   `<tr><td>Max (sensor ${b.tempMaxNo})</td><td>${f(b.tempMax,1)} C</td></tr>`+
   `<tr><td>Average</td><td>${f(b.tempAvg,1)} C</td></tr>`+
   `<tr><td>Min (sensor ${b.tempMinNo})</td><td>${f(b.tempMin,1)} C</td></tr>`+
   `<tr><td>MOSFET</td><td>${f(b.tempMos,1)} C</td></tr>`;

  document.getElementById('binfo').innerHTML=
   `<tr><td>Max cell</td><td>${f(b.cellMax,3)} V (cell ${b.cellMaxNo})</td></tr>`+
   `<tr><td>Min cell</td><td>${f(b.cellMin,3)} V (cell ${b.cellMinNo})</td></tr>`+
   `<tr><td>Cycles</td><td>${b.cycles}</td></tr>`+
   `<tr><td>SOH</td><td>${b.soh} %</td></tr>`;

  const s=j.bridge, rate=s.reqRx?100*s.bmsOk/s.reqRx:0;
  document.getElementById('st').innerHTML=
   `<tr><td>Uptime</td><td>${s.uptime}</td></tr>`+
   `<tr><td>Inverter requests</td><td>${s.reqRx}</td></tr>`+
   `<tr><td>Live BMS answers</td><td class="${rate>95?'ok':rate>80?'warn':'bad'}">${s.bmsOk} (${rate.toFixed(1)}%)</td></tr>`+
   `<tr><td>BMS timeouts</td><td>${s.bmsTimeout}</td></tr>`+
   `<tr><td>Served from cache</td><td>${s.cacheHits}</td></tr>`+
   `<tr><td>WiFi RSSI</td><td>${s.rssi} dBm</td></tr>`;

  document.getElementById('raw').textContent=
   '61: '+j.raw.c61+'\n62: '+j.raw.c62+'\n63: '+j.raw.c63;
  document.getElementById('upd').textContent='updated '+b.age+' s ago';
 }catch(e){document.getElementById('upd').textContent='no connection';}
}
tick();setInterval(tick,3000);
</script></body></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleApi() {
  uint32_t up = (millis() - bootMs) / 1000; char ub[16];
  snprintf(ub, sizeof(ub), "%lu:%02lu:%02lu", up / 3600, (up / 60) % 60, up % 60);
  String j = "{\"battery\":{";
  j += "\"voltage\":"   + String(bat.voltage, 3);
  j += ",\"current\":"  + String(bat.current, 2);
  j += ",\"power\":"    + String(bat.voltage * bat.current, 0);
  j += ",\"soc\":"      + String(bat.soc);
  j += ",\"soh\":"      + String(bat.soh);
  j += ",\"cycles\":"   + String(bat.cycles);
  j += ",\"cellMax\":"  + String(bat.cellMax, 3);
  j += ",\"cellMaxNo\":" + String(bat.cellMaxNo);
  j += ",\"cellMin\":"  + String(bat.cellMin, 3);
  j += ",\"cellMinNo\":" + String(bat.cellMinNo);
  j += ",\"delta\":"    + String((bat.cellMax - bat.cellMin) * 1000, 0);
  j += ",\"tempAvg\":"  + String(bat.tempAvg, 1);
  j += ",\"tempMax\":"  + String(bat.tempMax, 1);
  j += ",\"tempMaxNo\":" + String(bat.tempMaxNo);
  j += ",\"tempMin\":"  + String(bat.tempMin, 1);
  j += ",\"tempMinNo\":" + String(bat.tempMinNo);
  j += ",\"tempMos\":"  + String(bat.tempMos, 1);
  j += ",\"age\":"      + String(bat.ts ? (millis() - bat.ts) / 1000 : 0);
  j += ",\"valid\":"    + String(bat.valid ? "true" : "false");
  j += "},\"bridge\":{";
  j += "\"uptime\":\"" + String(ub) + "\"";
  j += ",\"reqRx\":"      + String(st.reqRx);
  j += ",\"reqBad\":"     + String(st.reqBad);
  j += ",\"bmsOk\":"      + String(st.bmsOk);
  j += ",\"bmsTimeout\":" + String(st.bmsTimeout);
  j += ",\"bmsBad\":"     + String(st.bmsBad);
  j += ",\"cacheHits\":"  + String(st.cacheHits);
  j += ",\"txInv\":"      + String(st.txInv);
  j += ",\"rssi\":"       + String(WiFi.RSSI());
  j += "},\"raw\":{";
  j += "\"c61\":\"" + bat.raw61 + "\",\"c62\":\"" + bat.raw62 + "\"";
  j += ",\"c63\":\"" + bat.raw63 + "\"}}";
  server.send(200, "application/json", j);
}

void handleLog() {
  String s = ""; int n = logIdx < LOG_LINES ? logIdx : LOG_LINES;
  for (int i = 0; i < n; i++) s += logBuf[(logIdx - n + i) % LOG_LINES] + "\n";
  server.send(200, "text/plain", s);
}

void handleRaw() {
  String s = "Raw payloads\n\n";
  s += "CID2=61 (analog data)        : " + bat.raw61 + "\n\n";
  s += "CID2=62 (alarms)             : " + bat.raw62 + "\n\n";
  s += "CID2=63 (charge/discharge)   : " + bat.raw63 + "\n\n";
  s += "CID2=61 map (big-endian, 49 bytes):\n";
  s += "  0-1 pack mV | 2-3 current /100 SIGNED | 4 SOC %\n";
  s += "  5-6 cycles | 9 SOH % | 11-12 max cell mV, 14 cell no | 15-16 min cell mV, 18 cell no\n";
  s += "  19-20 temp avg | 21-22 temp max, 24 sensor | 25-26 temp min, 28 sensor\n";
  s += "  29-30 MOSFET temp (0.1 K)\n\n";
  server.send(200, "text/plain", s);
}

// ---------- Browser OTA ----------
const char UPDATE_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware update</title><style>
body{background:#0d1117;color:#e6edf3;font:15px/1.5 -apple-system,system-ui,sans-serif;
padding:20px;max-width:520px;margin:0 auto}
h1{font-size:18px}.box{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:16px}
input[type=file]{width:100%;margin:10px 0;color:#8b949e}
button{background:#238636;color:#fff;border:0;border-radius:6px;padding:10px 18px;font-size:15px;width:100%}
button:disabled{background:#30363d}progress{width:100%;height:10px;margin-top:12px}
.msg{margin-top:10px;font-size:14px}a{color:#58a6ff;text-decoration:none}
</style></head><body><h1>Firmware update</h1><div class="box">
<p style="color:#8b949e;font-size:13px;margin-top:0">Arduino IDE: Sketch &rarr; Export Compiled Binary, then upload the .bin here.</p>
<input type="file" id="f" accept=".bin"><button id="b" onclick="upload()">Upload and install</button>
<progress id="pr" value="0" max="100" style="display:none"></progress><div class="msg" id="m"></div></div>
<p style="margin-top:16px"><a href="/">&larr; back</a></p><script>
function upload(){const f=document.getElementById('f').files[0];
if(!f){document.getElementById('m').textContent='Choose a .bin file';return;}
const b=document.getElementById('b'),pr=document.getElementById('pr'),m=document.getElementById('m');
b.disabled=true;pr.style.display='block';m.textContent='Uploading...';
const fd=new FormData();fd.append('update',f);const x=new XMLHttpRequest();
x.upload.onprogress=e=>{if(e.lengthComputable)pr.value=100*e.loaded/e.total;};
x.onload=()=>{if(x.status==200){m.style.color='#3fb950';m.textContent='Done, rebooting...';
setTimeout(()=>location.href='/',6000);}else{m.style.color='#f85149';m.textContent='Error';b.disabled=false;}};
x.onerror=()=>{m.style.color='#f85149';m.textContent='Connection failed';b.disabled=false;};
x.open('POST','/update');x.send(fd);}
</script></body></html>)HTML";

void handleUpdatePage() { server.send_P(200, "text/html", UPDATE_PAGE); }
void handleUpdateDone() {
  bool e = Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(e ? 500 : 200, "text/plain", e ? "failed" : "ok");
  if (!e) { delay(500); ESP.restart(); }
}
void handleUpdateUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START)      { led(255, 180, 0); Update.begin(UPDATE_SIZE_UNKNOWN); }
  else if (u.status == UPLOAD_FILE_WRITE) { Update.write(u.buf, u.currentSize); }
  else if (u.status == UPLOAD_FILE_END)   { Update.end(true) ? led(0, 255, 0) : led(255, 0, 0); }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200); delay(300);
  bootMs = millis(); lastReqMs = millis();
  led(255, 0, 0);                                    // red = booting

  pinMode(INV_DE, OUTPUT); digitalWrite(INV_DE, LOW);
  pinMode(BMS_DE, OUTPUT); digitalWrite(BMS_DE, LOW);
  SerialInv.begin(BAUD, SERIAL_8N1, INV_RX, INV_TX);
  SerialBms.begin(BAUD, SERIAL_8N1, BMS_RX, BMS_TX);

  Serial.println("\nFelicity <-> JK BMS Pylon bridge + web dashboard");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: http://"); Serial.println(WiFi.localIP());
    Serial.printf("mDNS: http://%s.local\n", OTA_HOST);
    addLog("WiFi OK " + WiFi.localIP().toString());
    ArduinoOTA.setHostname(OTA_HOST); ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]() { led(255, 180, 0); });
    ArduinoOTA.onEnd([]() { led(0, 255, 0); });
    ArduinoOTA.onError([](ota_error_t) { led(255, 0, 0); });
    ArduinoOTA.begin();
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("WiFi FAILED - the bridge keeps working without it");
    addLog("WiFi failed");
  }

  server.on("/", handleRoot);
  server.on("/api", handleApi);
  server.on("/log", handleLog);
  server.on("/raw", handleRaw);
  server.on("/update-ui", handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();
  led(0, 0, 40);                                     // dim blue = waiting for inverter
}

// ---------- Main loop ----------
// LED: blue = idle, green = live BMS answer, amber = answer from cache,
//      red = no answer, magenta = corrupt request from inverter
void loop() {
  server.handleClient(); ArduinoOTA.handle();

  String req = readFrame(SerialInv, 800);            // wait for an inverter request
  if (!req.length()) { led(0, 0, 40); return; }

  lastReqMs = millis(); st.reqRx++;
  if (!isValid(req)) { st.reqBad++; led(255, 0, 255); addLog("INV corrupt frame"); return; }

  String ver = req.substring(1, 3), adrIn = req.substring(3, 5), cid2 = req.substring(7, 9);
  String rest = req.substring(5, req.length() - 5);  // CID1..INFO (no ADR, no checksum)

  // Forward to the BMS with the address it understands
  delay(FRAME_GAP);
  sendFrame(SerialBms, BMS_DE, seal(ver + hex2(BMS_ADDR) + rest));
  String resp = readFrame(SerialBms, BMS_TIMEOUT);
  if (resp.length() && !isValid(resp)) { st.bmsBad++; addLog("BMS corrupt frame"); resp = ""; }

  String body; bool fromCache = false;

  if (resp.length()) {
    String c = resp.substring(1, resp.length() - 5); // VER ADR CID1 RTN LENGTH INFO
    lastBody = c.substring(0, 2) + c.substring(4);   // remember it without the ADR
    lastBodyMs = millis();
    body = c; st.bmsOk++; led(0, 255, 0);
    String info = resp.length() > 18 ? resp.substring(13, resp.length() - 5) : "";
    if      (cid2 == "61") decode61(info);
    else if (cid2 == "62") bat.raw62 = info;
    else if (cid2 == "63") bat.raw63 = info;
  } else if (lastBody.length() && millis() - lastBodyMs < CACHE_TTL) {
    st.bmsTimeout++; st.cacheHits++;
    body = lastBody.substring(0, 2) + hex2(BMS_ADDR) + lastBody.substring(2);
    fromCache = true; led(255, 180, 0);
  } else {
    st.bmsTimeout++; st.noAnswer++; led(255, 0, 0);
    addLog("no answer and no cache for adr " + adrIn); return;
  }

  // Answer the inverter using the address it asked for
  delay(FRAME_GAP);
  sendFrame(SerialInv, INV_DE, seal(body.substring(0, 2) + adrIn + body.substring(4)));
  st.txInv++;
  if (fromCache) addLog("adr " + adrIn + " cid2 " + cid2 + " [cache]");
}
