// ALTHERA - Fonte de tensão inteligente
// ESP32-S3-WROOM-2-N16R8 | PlatformIO + Arduino
//
// Funções:
//  1) Corrente de carga da bateria programável (2A / 4A / 10A) via GPIO41 / GPIO40
//  2) Leitura de tensão, corrente e potência de todos os canais (4x INA3221)
//  3) API HTTP/JSON (com CORS) para o dashboard HTML na nuvem conversar com cada placa
//
// Endpoints:
//   GET  /api/status                      -> tudo em JSON
//   GET  /api/charge                      -> modo de carga atual
//   POST /api/charge   {"mode":"medium"}  -> slow | medium | fast   (ou {"amps":4})

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "ina3221.h"

// =====================================================================
//  CARGA DA BATERIA
//  Slow   : 41=LOW  40=LOW   -> só R21 (20mΩ)          -> 2A
//  Medium : 41=HIGH 40=LOW   -> 20mΩ || 20mΩ = 10mΩ    -> 4A
//  Fast   : 41=LOW  40=HIGH  -> 20mΩ || 5mΩ  = 4mΩ     -> 10A
//  HIGH+HIGH é PROIBIDO (~12A, fora do projeto) e nunca é aplicado.
// =====================================================================
enum ChargeMode : uint8_t { CHG_SLOW = 0, CHG_MEDIUM = 1, CHG_FAST = 2 };
static const char  *MODE_NAMES[] = {"slow", "medium", "fast"};
static const float  MODE_AMPS[]  = {2.0f, 4.0f, 10.0f};
static ChargeMode   chargeMode   = CHG_SLOW;

static void applyChargeMode(ChargeMode m) {
  // Sempre passa por LOW/LOW antes: os dois pinos nunca ficam HIGH juntos.
  digitalWrite(PIN_CHG_4A, LOW);
  digitalWrite(PIN_CHG_10A, LOW);
  delay(5);
  if (m == CHG_MEDIUM) digitalWrite(PIN_CHG_4A, HIGH);
  if (m == CHG_FAST)   digitalWrite(PIN_CHG_10A, HIGH);
  chargeMode = m;
  Serial.printf("[CHG] modo=%s (%.0fA)\n", MODE_NAMES[m], MODE_AMPS[m]);
}

// =====================================================================
//  CANAIS MONITORADOS
//  Shunts: entradas 5mΩ/3W | 12V 5mΩ/2W | 24V 10mΩ/1W | 5V e 9V 10mΩ/1W
//  sign: use -1 se o shunt estiver montado ao contrário (corrente negativa)
// =====================================================================
enum Role : uint8_t { ROLE_SOURCE, ROLE_BATTERY, ROLE_LOAD };

struct Channel {
  const char *name;
  Role        role;
  uint8_t     chip;      // índice em chips[]
  uint8_t     ch;        // IN1..IN3 do INA3221
  float       r_mOhm;    // resistor shunt
  int8_t      sign;
  bool        enabled;
  // medições
  float v, i, p;
  bool  valid;
};

static INA3221 chips[4] = {INA3221(0x40), INA3221(0x41), INA3221(0x42), INA3221(0x43)};
static const char *CHIP_REF[4] = {"U27", "U26", "U25", "U28"};
static bool chipOk[4] = {false, false, false, false};

static Channel channels[] = {
  // --- U27 (0x40): entradas ---
  {"BATERIA",  ROLE_BATTERY, 0, 1,  5, +1, true},
  {"SOLAR",    ROLE_SOURCE,  0, 2,  5, +1, true},
  {"FONTE",    ROLE_SOURCE,  0, 3,  5, +1, true},
  // --- U26 (0x41): saídas 12V ch1/ch2 + 24V ---
  {"24V",      ROLE_LOAD,    1, 1, 10, +1, true},
  {"12V_CH1",  ROLE_LOAD,    1, 2,  5, +1, true},
  {"12V_CH2",  ROLE_LOAD,    1, 3,  5, +1, true},
  // --- U25 (0x42): saídas 12V ch3/ch4/ch5 ---
  {"12V_CH3",  ROLE_LOAD,    2, 1,  5, +1, true},
  {"12V_CH4",  ROLE_LOAD,    2, 2,  5, +1, true},
  {"12V_CH5",  ROLE_LOAD,    2, 3,  5, +1, true},
  // --- U28 (0x43): saídas 5V e 9V  (!! ajuste os nomes conforme o esquemático) ---
  {"5V_1",     ROLE_LOAD,    3, 1, 10, +1, true},
  {"5V_2",     ROLE_LOAD,    3, 2, 10, +1, true},
  {"9V",       ROLE_LOAD,    3, 3, 10, +1, true},
};
static const size_t N_CH = sizeof(channels) / sizeof(channels[0]);

static void sampleAll() {
  for (size_t k = 0; k < N_CH; k++) {
    Channel &c = channels[k];
    float busV, shunt_mV;
    if (!c.enabled || !chipOk[c.chip] || !chips[c.chip].readChannel(c.ch, busV, shunt_mV)) {
      c.valid = false;
      continue;
    }
    c.v = busV;
    c.i = c.sign * shunt_mV / c.r_mOhm;   // mV / mΩ = A
    c.p = c.v * c.i;                      // W
    c.valid = true;
  }
}

static void initChips() {
  for (int n = 0; n < 4; n++) {
    if (!chipOk[n]) {
      chipOk[n] = chips[n].begin();
      Serial.printf("[INA] %s @0x%02X: %s\n", CHIP_REF[n], chips[n].address(), chipOk[n] ? "OK" : "NAO ENCONTRADO");
    }
  }
}

// =====================================================================
//  JSON
// =====================================================================
#if WIFI_ENABLED   // ---- tudo daqui até o próximo #endif só existe com WiFi ----
static inline float r3(float x) { return roundf(x * 1000.0f) / 1000.0f; }

static String buildStatusJson() {
  JsonDocument doc;
  doc["device"]   = DEVICE_ID;
  doc["uptime_s"] = millis() / 1000;
  doc["ip"]       = WiFi.localIP().toString();
  doc["rssi"]     = WiFi.RSSI();

  JsonObject chg = doc["charge"].to<JsonObject>();
  chg["mode"]      = MODE_NAMES[chargeMode];
  chg["current_a"] = MODE_AMPS[chargeMode];

  JsonArray jc = doc["chips"].to<JsonArray>();
  for (int n = 0; n < 4; n++) {
    JsonObject o = jc.add<JsonObject>();
    o["ref"] = CHIP_REF[n];
    char a[8]; snprintf(a, sizeof(a), "0x%02X", chips[n].address());
    o["addr"] = a;
    o["online"] = chipOk[n];
  }

  float pSrc = 0, pLoad = 0, pBat = 0;
  JsonArray ja = doc["channels"].to<JsonArray>();
  for (size_t k = 0; k < N_CH; k++) {
    const Channel &c = channels[k];
    JsonObject o = ja.add<JsonObject>();
    o["name"] = c.name;
    o["role"] = c.role == ROLE_SOURCE ? "source" : c.role == ROLE_BATTERY ? "battery" : "load";
    o["chip"] = CHIP_REF[c.chip];
    o["valid"] = c.valid;
    if (c.valid) {
      o["v"] = r3(c.v);
      o["i"] = r3(c.i);
      o["p"] = r3(c.p);
      if (c.role == ROLE_SOURCE)  pSrc  += c.p;
      if (c.role == ROLE_LOAD)    pLoad += c.p;
      if (c.role == ROLE_BATTERY) pBat  += c.p;
    }
  }

  JsonObject t = doc["totals"].to<JsonObject>();
  t["sources_w"] = r3(pSrc);
  t["loads_w"]   = r3(pLoad);
  t["battery_w"] = r3(pBat);

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================================
//  HTTP
// =====================================================================
static WebServer server(80);

static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,X-Api-Key");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}

static void sendJson(int code, const String &body) {
  cors();
  server.send(code, "application/json", body);
}

static String chargeJson() {
  JsonDocument d;
  d["mode"] = MODE_NAMES[chargeMode];
  d["current_a"] = MODE_AMPS[chargeMode];
  String s; serializeJson(d, s); return s;
}

static void handleStatus() { sendJson(200, buildStatusJson()); }

static void handleCharge() {
  if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
  if (server.method() == HTTP_GET)     { sendJson(200, chargeJson()); return; }
  if (server.method() != HTTP_POST)    { sendJson(405, "{\"error\":\"metodo\"}"); return; }

  if (strlen(API_TOKEN) > 0 && server.header("X-Api-Key") != API_TOKEN) {
    sendJson(401, "{\"error\":\"nao autorizado\"}");
    return;
  }

  String mode = server.arg("mode");
  float amps = server.arg("amps").toFloat();
  if (server.hasArg("plain")) {
    JsonDocument d;
    if (!deserializeJson(d, server.arg("plain"))) {
      if (d["mode"].is<const char *>()) mode = d["mode"].as<String>();
      if (d["amps"].is<float>())        amps = d["amps"].as<float>();
    }
  }

  int idx = -1;
  for (int m = 0; m < 3; m++) if (mode == MODE_NAMES[m]) idx = m;
  if (idx < 0 && amps > 0) {
    if (amps == 2) idx = CHG_SLOW; else if (amps == 4) idx = CHG_MEDIUM; else if (amps == 10) idx = CHG_FAST;
  }
  if (idx < 0) {
    sendJson(400, "{\"error\":\"use mode=slow|medium|fast ou amps=2|4|10\"}");
    return;
  }
  applyChargeMode((ChargeMode)idx);
  sendJson(200, chargeJson());
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
  sendJson(404, "{\"error\":\"nao encontrado\"}");
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] conectando");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\n[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\n[WIFI] sem conexao (segue tentando em background)");
}
#endif  // WIFI_ENABLED

// Comandos pela serial (funcionam sem WiFi): s = 2A | m = 4A | f = 10A
static void handleSerialCommands() {
  while (Serial.available()) {
    char ch = Serial.read();
    if      (ch == 's') applyChargeMode(CHG_SLOW);
    else if (ch == 'm') applyChargeMode(CHG_MEDIUM);
    else if (ch == 'f') applyChargeMode(CHG_FAST);
  }
}

static void printLog() {
#if LOG_MODE == 1
  Serial.printf("--- carga: %s (%.0fA) ---\n", MODE_NAMES[chargeMode], MODE_AMPS[chargeMode]);
  for (int n = 0; n < 4; n++)
    if (!chipOk[n]) Serial.printf("!! %s (0x%02X) OFFLINE\n", CHIP_REF[n], chips[n].address());
  for (size_t k = 0; k < N_CH; k++) {
    const Channel &c = channels[k];
    if (c.valid) Serial.printf("%-8s %6.2f V  %7.3f A  %7.2f W\n", c.name, c.v, c.i, c.p);
    else         Serial.printf("%-8s ---\n", c.name);
  }
  Serial.println();
#elif LOG_MODE == 2
  // Formato Teleplot: >nome:valor  (uma linha por variável)
  for (size_t k = 0; k < N_CH; k++) {
    const Channel &c = channels[k];
    if (!c.valid) continue;
    Serial.printf(">%s_V:%.3f\n>%s_A:%.3f\n>%s_W:%.3f\n", c.name, c.v, c.name, c.i, c.name, c.p);
  }
  Serial.printf(">carga_A:%.0f\n", MODE_AMPS[chargeMode]);
#elif LOG_MODE == 3
  static bool header = false;
  if (!header) {
    header = true;
    Serial.print("ms");
    for (size_t k = 0; k < N_CH; k++) Serial.printf(",%s_V,%s_A,%s_W", channels[k].name, channels[k].name, channels[k].name);
    Serial.println();
  }
  Serial.print(millis());
  for (size_t k = 0; k < N_CH; k++) {
    const Channel &c = channels[k];
    if (c.valid) Serial.printf(",%.3f,%.3f,%.3f", c.v, c.i, c.p);
    else         Serial.print(",,,");
  }
  Serial.println();
#endif
}

void setup() {
  // Pinos de carga PRIMEIRO, em LOW (2A = estado seguro)
  pinMode(PIN_CHG_4A, OUTPUT);
  pinMode(PIN_CHG_10A, OUTPUT);
  digitalWrite(PIN_CHG_4A, LOW);
  digitalWrite(PIN_CHG_10A, LOW);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // espera a USB nativa abrir
  Serial.println("\n=== ALTHERA ===");
  Serial.println("Comandos: s = 2A | m = 4A | f = 10A");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  initChips();

#if WIFI_ENABLED
  connectWifi();
  if (MDNS.begin(DEVICE_ID)) MDNS.addService("http", "tcp", 80);

  const char *hdrs[] = {"X-Api-Key"};
  server.collectHeaders(hdrs, 1);
  server.on("/api/status", HTTP_ANY, []() {
    if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
    handleStatus();
  });
  server.on("/api/charge", HTTP_ANY, handleCharge);
  server.onNotFound(handleNotFound);
  server.begin();
#else
  Serial.println("[WIFI] desligado (WIFI_ENABLED 0) - modo debug serial");
#endif
}

void loop() {
#if WIFI_ENABLED
  server.handleClient();
#endif
  handleSerialCommands();

  static uint32_t tSample = 0, tRetry = 0, tLog = 0;
  uint32_t now = millis();

  if (now - tSample >= SAMPLE_INTERVAL_MS) { tSample = now; sampleAll(); }
  if (now - tRetry  >= INA_RETRY_MS)       { tRetry = now; initChips(); }
  if (now - tLog    >= LOG_INTERVAL_MS)    { tLog = now; printLog(); }
}