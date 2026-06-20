#include <Adafruit_ADS1X15.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include "coral_wallpaper.h"
#include "secrets.h"

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "AquaPH-Controller"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "aquaph1234"
#endif

#ifndef WIFI_OTA_PASSWORD
#define WIFI_OTA_PASSWORD WIFI_AP_PASSWORD
#endif

namespace {

constexpr uint8_t kSdaPin = 21;
constexpr uint8_t kSclPin = 22;
constexpr uint8_t kRelayPin = 26;
constexpr uint8_t kAdsAddress = 0x48;
constexpr uint8_t kAdsChannel = 0;
constexpr bool kRelayActiveLow = false;

constexpr float kDividerScale = 28.0f / 18.0f;  // 10k series, 18k to ground.
constexpr uint32_t kSampleIntervalMs = 500;
constexpr uint32_t kRelaySettleMs = 3000;
constexpr uint32_t kWifiRetryMs = 15000;
constexpr uint16_t kOtaPort = 3232;
constexpr size_t kMedianSamples = 15;
constexpr float kEmaAlpha = 0.22f;
constexpr float kMaxBatchJumpMv = 180.0f;

struct Settings {
  float ph4Mv = 3000.0f;
  float ph7Mv = 2500.0f;
  float valveOpenPh = 6.70f;
  float valveClosePh = 6.50f;
  bool autoEnabled = false;
  bool ph4Calibrated = false;
  bool ph7Calibrated = false;
};

Adafruit_ADS1115 ads;
Preferences preferences;
WebServer server(80);
Settings settings;

bool adsReady = false;
bool relayOn = false;
bool hasFilteredMv = false;
bool sampleSettling = false;
bool mdnsReady = false;
bool apReady = false;
bool otaReady = false;
bool otaInProgress = false;
uint8_t rejectedBatchCount = 0;
float rawMv = 0.0f;
float medianMv = 0.0f;
float filteredMv = 0.0f;
float phValue = 0.0f;
uint32_t lastSampleAt = 0;
uint32_t relayChangedAt = 0;
uint32_t lastWifiAttemptAt = 0;
String serialInput;

const char kPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>钙反应 pH 控制器</title>
  <style>
    :root{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#17212b;background:#061115}
    *{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(rgba(4,18,23,.72),rgba(4,18,23,.82)),url('/wallpaper.jpg') center/cover fixed no-repeat}.top{background:rgba(10,35,45,.86);color:#fff;padding:18px 16px;border-bottom:1px solid rgba(255,255,255,.16);box-shadow:0 8px 24px rgba(0,0,0,.18)}.top h1{font-size:22px;margin:0;letter-spacing:0}.top p{margin:5px 0 0;color:#c8dde2;font-size:13px}
    main{max-width:980px;margin:auto;padding:14px}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.panel{background:rgba(255,255,255,.88);border:1px solid rgba(255,255,255,.55);border-radius:10px;padding:14px;margin-bottom:12px;box-shadow:0 10px 30px rgba(0,0,0,.16);-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px)}.metric{min-height:116px}.label{color:#60727a;font-size:13px}.value{font-size:31px;font-weight:700;margin-top:9px}.unit{font-size:16px;color:#60727a}.row{display:flex;gap:9px;align-items:center;flex-wrap:wrap}.between{justify-content:space-between}.status{display:inline-block;width:10px;height:10px;border-radius:50%;background:#a4afb3;margin-right:7px}.ok{background:#1f9d68}.warn{background:#d58a12}.bad{background:#d94b4b}button{border:0;border-radius:6px;padding:12px 16px;font-size:15px;font-weight:650;cursor:pointer;background:#e5ebed;color:#17212b}button.primary{background:#126e82;color:#fff}button.danger{background:#c94848;color:#fff}button.good{background:#268b62;color:#fff}button:disabled{opacity:.5}.field{display:grid;grid-template-columns:1fr 130px;gap:10px;align-items:center;margin:10px 0}.field input{width:100%;border:1px solid #b8c7cc;border-radius:5px;padding:10px;font-size:16px}.switch{display:flex;gap:8px;margin-top:12px}.switch button{flex:1}.note{font-size:13px;color:#65757b;line-height:1.55}.msg{font-size:13px;color:#126e82;min-height:18px;margin-top:8px}.hidden{display:none}@media(max-width:680px){.grid{grid-template-columns:1fr}.metric{min-height:86px}.value{font-size:27px}}
  </style>
</head>
<body>
<header class="top"><h1>钙反应 pH 控制器</h1><p>ESP32 本地控制面板</p></header>
<main>
  <section class="grid">
    <div class="panel metric"><div class="label">当前 pH</div><div class="value" id="ph">--</div></div>
    <div class="panel metric"><div class="label">滤波后电压</div><div class="value"><span id="mv">--</span> <span class="unit">mV</span></div></div>
    <div class="panel metric"><div class="label">继电器</div><div class="value" id="relay">--</div></div>
  </section>
  <section class="panel">
    <div class="row between"><strong>运行状态</strong><span><i class="status" id="wifiDot"></i><span id="wifi">--</span></span></div>
    <p class="note" id="detail">正在读取...</p>
  </section>
  <section class="panel">
    <strong>控制模式</strong>
    <div class="switch">
      <button id="manualBtn" onclick="setMode(false)">手动模式</button>
      <button id="autoBtn" onclick="setMode(true)">自动模式</button>
    </div>
    <div class="switch">
      <button class="good" onclick="relay(true)">手动开启阀门</button>
      <button class="danger" onclick="relay(false)">手动关闭阀门</button>
    </div>
    <p class="note">自动模式下，pH 高于开启值时打开阀门；pH 低于关闭值时关闭阀门。开启值必须高于关闭值。</p>
  </section>
  <section class="panel">
    <strong>自动控制阈值</strong>
    <label class="field"><span>开启阀门 pH</span><input id="openPh" type="number" min="0" max="14" step="0.01"></label>
    <label class="field"><span>关闭阀门 pH</span><input id="closePh" type="number" min="0" max="14" step="0.01"></label>
    <button class="primary" onclick="saveThresholds()">保存阈值</button>
  </section>
  <section class="panel">
    <strong>pH 两点校准</strong>
    <p class="note">将探头放入标准液，等电压稳定后点击对应按钮。校准完成后会自动保存。</p>
    <div class="switch">
      <button onclick="calibrate(7)">记录 pH 7.00 标准液</button>
      <button onclick="calibrate(4)">记录 pH 4.00 标准液</button>
    </div>
    <p class="note">已记录：pH 7.00 = <span id="ph7">--</span> mV；pH 4.00 = <span id="ph4">--</span> mV</p>
    <button onclick="resetCalibration()">恢复默认校准</button>
    <div class="msg" id="msg"></div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id);
let cfgLoaded=false;
async function api(path,opt){const r=await fetch(path,opt);const j=await r.json();if(!r.ok)throw Error(j.error||'请求失败');return j}
function say(s){$('msg').textContent=s;setTimeout(()=>{if($('msg').textContent===s)$('msg').textContent=''},4000)}
async function refresh(){
 try{
  const s=await api('/api/status');
  $('ph').textContent=s.ph_valid?s.ph.toFixed(2):'--';
  $('mv').textContent=s.mv_valid?s.filtered_mv.toFixed(1):'--';
  $('relay').textContent=s.relay_on?'开启':'关闭';
  $('relay').style.color=s.relay_on?'#268b62':'#c94848';
  $('wifi').textContent=s.wifi_connected?`局域网 ${s.ip} · ${s.rssi} dBm`:(s.ap_ready?`热点 ${s.ap_ssid} · ${s.ap_ip}`:'网络未连接');
  $('wifiDot').className='status '+((s.wifi_connected||s.ap_ready)?'ok':'bad');
  $('detail').textContent=`ADS1115：${s.ads_ready?'正常':'未检测到'}；热点：${s.ap_ready?`${s.ap_ssid} ${s.ap_ip}`:'未开启'}；OTA：${s.ota_ready?'已开启':'未开启'}；原始 ${s.raw_mv.toFixed(1)} mV；中值 ${s.median_mv.toFixed(1)} mV${s.settling?'；继电器动作后稳定等待中':''}`;
  $('manualBtn').className=s.auto_enabled?'':'primary';$('autoBtn').className=s.auto_enabled?'primary':'';
  if(!cfgLoaded){$('openPh').value=s.open_ph.toFixed(2);$('closePh').value=s.close_ph.toFixed(2);cfgLoaded=true}
  $('ph7').textContent=s.ph7_mv.toFixed(1);$('ph4').textContent=s.ph4_mv.toFixed(1);
 }catch(e){$('wifi').textContent='无法连接设备';$('wifiDot').className='status bad'}
}
async function post(path,data={}){try{const j=await api(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});say(j.message||'已保存');cfgLoaded=false;refresh()}catch(e){say(e.message)}}
function relay(on){post('/api/relay',{on:on?'1':'0'})}
function setMode(auto){post('/api/mode',{auto:auto?'1':'0'})}
function saveThresholds(){post('/api/settings',{open_ph:$('openPh').value,close_ph:$('closePh').value})}
function calibrate(ph){post('/api/calibrate',{point:String(ph)})}
function resetCalibration(){if(confirm('恢复默认校准参数？'))post('/api/calibrate/reset')}
refresh();setInterval(refresh,1500);
</script>
</body>
</html>
)HTML";

String boolJson(bool value) {
  return value ? "true" : "false";
}

float calculatePh(float mv) {
  const float deltaMv = settings.ph7Mv - settings.ph4Mv;
  if (fabsf(deltaMv) < 10.0f) {
    return NAN;
  }
  return 7.0f + (mv - settings.ph7Mv) * (7.0f - 4.0f) / deltaMv;
}

void saveSettings() {
  preferences.putFloat("ph4_mv", settings.ph4Mv);
  preferences.putFloat("ph7_mv", settings.ph7Mv);
  preferences.putFloat("open_ph", settings.valveOpenPh);
  preferences.putFloat("close_ph", settings.valveClosePh);
  preferences.putBool("auto", settings.autoEnabled);
  preferences.putBool("ph4_done", settings.ph4Calibrated);
  preferences.putBool("ph7_done", settings.ph7Calibrated);
}

void loadSettings() {
  preferences.begin("aquaph", false);
  if (preferences.isKey("ph4_mv")) settings.ph4Mv = preferences.getFloat("ph4_mv");
  if (preferences.isKey("ph7_mv")) settings.ph7Mv = preferences.getFloat("ph7_mv");
  if (preferences.isKey("open_ph")) settings.valveOpenPh = preferences.getFloat("open_ph");
  if (preferences.isKey("close_ph")) settings.valveClosePh = preferences.getFloat("close_ph");
  if (preferences.isKey("auto")) settings.autoEnabled = preferences.getBool("auto");
  if (preferences.isKey("ph4_done")) settings.ph4Calibrated = preferences.getBool("ph4_done");
  if (preferences.isKey("ph7_done")) settings.ph7Calibrated = preferences.getBool("ph7_done");
}

void setRelay(bool on) {
  if (relayOn == on) {
    return;
  }
  relayOn = on;
  digitalWrite(kRelayPin, (relayOn != kRelayActiveLow) ? HIGH : LOW);
  relayChangedAt = millis();
  Serial.printf("Relay: %s\n", relayOn ? "ON" : "OFF");
}

void updateAutomaticControl() {
  if (!settings.autoEnabled || isnan(phValue) || sampleSettling) {
    return;
  }
  if (!relayOn && phValue >= settings.valveOpenPh) {
    setRelay(true);
  } else if (relayOn && phValue <= settings.valveClosePh) {
    setRelay(false);
  }
}

float readMedianMv() {
  float values[kMedianSamples];
  for (size_t i = 0; i < kMedianSamples; ++i) {
    const int16_t raw = ads.readADC_SingleEnded(kAdsChannel);
    values[i] = ads.computeVolts(raw) * 1000.0f * kDividerScale;
    rawMv = values[i];
    delay(9);
  }
  for (size_t i = 1; i < kMedianSamples; ++i) {
    const float value = values[i];
    size_t j = i;
    while (j > 0 && values[j - 1] > value) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = value;
  }
  return values[kMedianSamples / 2];
}

void samplePh() {
  if (!adsReady || millis() - lastSampleAt < kSampleIntervalMs) {
    return;
  }
  lastSampleAt = millis();
  medianMv = readMedianMv();
  sampleSettling = millis() - relayChangedAt < kRelaySettleMs;

  if (!hasFilteredMv) {
    filteredMv = medianMv;
    hasFilteredMv = true;
  } else if (!sampleSettling) {
    const bool looksLikeSpike = fabsf(medianMv - filteredMv) > kMaxBatchJumpMv;
    if (!looksLikeSpike) {
      rejectedBatchCount = 0;
      filteredMv += kEmaAlpha * (medianMv - filteredMv);
    } else if (++rejectedBatchCount >= 4) {
      // Accept a sustained step change, such as moving the probe to calibration fluid.
      rejectedBatchCount = 0;
      filteredMv = medianMv;
    }
  }
  phValue = calculatePh(filteredMv);
  updateAutomaticControl();
}

String statusJson() {
  String json = "{";
  json += "\"ads_ready\":" + boolJson(adsReady);
  json += ",\"wifi_connected\":" + boolJson(WiFi.status() == WL_CONNECTED);
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  json += ",\"ap_ready\":" + boolJson(apReady);
  json += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"ap_ssid\":\"" + String(WIFI_AP_SSID) + "\"";
  json += ",\"ota_ready\":" + boolJson(otaReady);
  json += ",\"ota_in_progress\":" + boolJson(otaInProgress);
  json += ",\"ota_hostname\":\"aquaph\"";
  json += ",\"ota_port\":" + String(kOtaPort);
  json += ",\"relay_on\":" + boolJson(relayOn);
  json += ",\"auto_enabled\":" + boolJson(settings.autoEnabled);
  json += ",\"settling\":" + boolJson(sampleSettling);
  json += ",\"mv_valid\":" + boolJson(hasFilteredMv);
  json += ",\"ph_valid\":" + boolJson(hasFilteredMv && !isnan(phValue));
  json += ",\"raw_mv\":" + String(rawMv, 2);
  json += ",\"median_mv\":" + String(medianMv, 2);
  json += ",\"filtered_mv\":" + String(filteredMv, 2);
  json += ",\"ph\":" + String(isnan(phValue) ? 0.0f : phValue, 3);
  json += ",\"ph7_mv\":" + String(settings.ph7Mv, 2);
  json += ",\"ph4_mv\":" + String(settings.ph4Mv, 2);
  json += ",\"ph7_calibrated\":" + boolJson(settings.ph7Calibrated);
  json += ",\"ph4_calibrated\":" + boolJson(settings.ph4Calibrated);
  json += ",\"calibrated\":" + boolJson(settings.ph7Calibrated && settings.ph4Calibrated);
  json += ",\"open_ph\":" + String(settings.valveOpenPh, 2);
  json += ",\"close_ph\":" + String(settings.valveClosePh, 2);
  json += ",\"uptime_ms\":" + String(millis());
  json += "}";
  return json;
}

void fillStatusDoc(JsonObject json) {
  json["ads_ready"] = adsReady;
  json["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  json["ip"] = WiFi.localIP().toString();
  json["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  json["ap_ready"] = apReady;
  json["ap_ip"] = WiFi.softAPIP().toString();
  json["ap_ssid"] = String(WIFI_AP_SSID);
  json["ota_ready"] = otaReady;
  json["ota_in_progress"] = otaInProgress;
  json["ota_hostname"] = "aquaph";
  json["ota_port"] = kOtaPort;
  json["relay_on"] = relayOn;
  json["auto_enabled"] = settings.autoEnabled;
  json["settling"] = sampleSettling;
  json["mv_valid"] = hasFilteredMv;
  json["ph_valid"] = hasFilteredMv && !isnan(phValue);
  json["raw_mv"] = rawMv;
  json["median_mv"] = medianMv;
  json["filtered_mv"] = filteredMv;
  json["ph"] = isnan(phValue) ? 0.0f : phValue;
  json["ph7_mv"] = settings.ph7Mv;
  json["ph4_mv"] = settings.ph4Mv;
  json["ph7_calibrated"] = settings.ph7Calibrated;
  json["ph4_calibrated"] = settings.ph4Calibrated;
  json["calibrated"] = settings.ph7Calibrated && settings.ph4Calibrated;
  json["open_ph"] = settings.valveOpenPh;
  json["close_ph"] = settings.valveClosePh;
  json["uptime_ms"] = millis();
}

void sendJson(int status, const String &json) {
  server.send(status, "application/json; charset=utf-8", json);
}

void sendMessage(const String &message) {
  sendJson(200, "{\"message\":\"" + message + "\"}");
}

void sendSerialResponse(const String &id, const String &message = "", float voltage = NAN) {
  StaticJsonDocument<2048> response;
  response["ok"] = true;
  if (id.length() > 0) {
    response["id"] = id;
  }
  if (message.length() > 0) {
    response["message"] = message;
  }
  if (!isnan(voltage)) {
    response["voltage"] = voltage;
  }
  JsonObject snapshot = response.createNestedObject("snapshot");
  fillStatusDoc(snapshot);
  serializeJson(response, Serial);
  Serial.println();
}

void sendSerialError(const String &id, const String &error) {
  StaticJsonDocument<384> response;
  response["ok"] = false;
  if (id.length() > 0) {
    response["id"] = id;
  }
  response["error"] = error;
  serializeJson(response, Serial);
  Serial.println();
}

void handleSerialCommandLine(const String &line) {
  StaticJsonDocument<768> request;
  const DeserializationError error = deserializeJson(request, line);
  if (error) {
    sendSerialError("", "invalid_json");
    return;
  }

  const String id = request["id"] | "";
  const String cmd = request["cmd"] | "";

  if (cmd == "hello") {
    StaticJsonDocument<768> response;
    response["ok"] = true;
    if (id.length() > 0) {
      response["id"] = id;
    }
    response["device"] = "aquaph-esp32";
    response["protocol"] = "aquaph-jsonl-v1";
    response["firmware"] = __DATE__ " " __TIME__;
    response["message"] = "ready";
    serializeJson(response, Serial);
    Serial.println();
    return;
  }

  if (cmd == "status" || cmd == "heartbeat") {
    sendSerialResponse(id);
    return;
  }

  if (cmd == "relay") {
    if (!request.containsKey("on") && !request.containsKey("state")) {
      sendSerialError(id, "missing_relay_state");
      return;
    }
    settings.autoEnabled = false;
    saveSettings();
    setRelay(request.containsKey("on") ? request["on"].as<bool>() : request["state"].as<bool>());
    sendSerialResponse(id, "manual relay updated");
    return;
  }

  if (cmd == "mode") {
    settings.autoEnabled = request["auto"].as<bool>();
    saveSettings();
    if (settings.autoEnabled) {
      updateAutomaticControl();
    }
    sendSerialResponse(id, settings.autoEnabled ? "auto mode enabled" : "manual mode enabled");
    return;
  }

  if (cmd == "settings") {
    const bool hasOpen = request.containsKey("open_ph");
    const bool hasClose = request.containsKey("close_ph");
    const float openPh = hasOpen ? request["open_ph"].as<float>() : settings.valveOpenPh;
    const float closePh = hasClose ? request["close_ph"].as<float>() : settings.valveClosePh;
    if (openPh <= closePh || openPh > 14.0f || closePh < 0.0f) {
      sendSerialError(id, "invalid_thresholds");
      return;
    }
    if (hasOpen) {
      settings.valveOpenPh = openPh;
    }
    if (hasClose) {
      settings.valveClosePh = closePh;
    }
    if (request.containsKey("auto_enabled")) {
      settings.autoEnabled = request["auto_enabled"].as<bool>();
    } else if (request.containsKey("enabled")) {
      settings.autoEnabled = request["enabled"].as<bool>();
    }
    saveSettings();
    if (settings.autoEnabled) {
      updateAutomaticControl();
    }
    sendSerialResponse(id, "settings saved");
    return;
  }

  if (cmd == "calibrate") {
    if (!hasFilteredMv || sampleSettling) {
      sendSerialError(id, "sample_not_stable");
      return;
    }
    const int point = request["point"] | 0;
    if (point == 7) {
      settings.ph7Mv = filteredMv;
      settings.ph7Calibrated = true;
    } else if (point == 4) {
      settings.ph4Mv = filteredMv;
      settings.ph4Calibrated = true;
    } else {
      sendSerialError(id, "invalid_calibration_point");
      return;
    }
    saveSettings();
    phValue = calculatePh(filteredMv);
    sendSerialResponse(id, "calibration saved", filteredMv / 1000.0f);
    return;
  }

  if (cmd == "calibrate_reset") {
    settings.ph7Mv = 2500.0f;
    settings.ph4Mv = 3000.0f;
    settings.ph7Calibrated = false;
    settings.ph4Calibrated = false;
    saveSettings();
    phValue = calculatePh(filteredMv);
    sendSerialResponse(id, "calibration reset");
    return;
  }

  sendSerialError(id, "unknown_cmd");
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      const String line = serialInput;
      serialInput = "";
      if (line.length() > 0) {
        handleSerialCommandLine(line);
      }
      continue;
    }
    if (serialInput.length() < 768) {
      serialInput += ch;
    } else {
      serialInput = "";
      sendSerialError("", "line_too_long");
    }
  }
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", kPage);
  });
  server.on("/wallpaper.jpg", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "public, max-age=604800");
    server.send_P(200, "image/jpeg", reinterpret_cast<const char *>(kCoralWallpaperJpg),
                  kCoralWallpaperJpgLen);
  });
  server.on("/api/status", HTTP_GET, []() {
    sendJson(200, statusJson());
  });
  server.on("/api/relay", HTTP_POST, []() {
    if (!server.hasArg("on")) {
      sendJson(400, "{\"error\":\"缺少继电器状态\"}");
      return;
    }
    settings.autoEnabled = false;
    saveSettings();
    setRelay(server.arg("on") == "1");
    sendMessage("已切换为手动模式");
  });
  server.on("/api/mode", HTTP_POST, []() {
    settings.autoEnabled = server.arg("auto") == "1";
    saveSettings();
    if (settings.autoEnabled) {
      updateAutomaticControl();
    }
    sendMessage(settings.autoEnabled ? "已启用自动模式" : "已启用手动模式");
  });
  server.on("/api/settings", HTTP_POST, []() {
    const float openPh = server.arg("open_ph").toFloat();
    const float closePh = server.arg("close_ph").toFloat();
    if (openPh <= closePh || openPh > 14.0f || closePh < 0.0f) {
      sendJson(400, "{\"error\":\"开启 pH 必须高于关闭 pH\"}");
      return;
    }
    settings.valveOpenPh = openPh;
    settings.valveClosePh = closePh;
    saveSettings();
    sendMessage("阈值已保存");
  });
  server.on("/api/calibrate", HTTP_POST, []() {
    if (!hasFilteredMv || sampleSettling) {
      sendJson(409, "{\"error\":\"请等待电压稳定后再校准\"}");
      return;
    }
    if (server.arg("point") == "7") {
      settings.ph7Mv = filteredMv;
      settings.ph7Calibrated = true;
    } else if (server.arg("point") == "4") {
      settings.ph4Mv = filteredMv;
      settings.ph4Calibrated = true;
    } else {
      sendJson(400, "{\"error\":\"未知校准点\"}");
      return;
    }
    saveSettings();
    phValue = calculatePh(filteredMv);
    sendMessage("校准点已记录");
  });
  server.on("/api/calibrate/reset", HTTP_POST, []() {
    settings.ph7Mv = 2500.0f;
    settings.ph4Mv = 3000.0f;
    settings.ph7Calibrated = false;
    settings.ph4Calibrated = false;
    saveSettings();
    phValue = calculatePh(filteredMv);
    sendMessage("已恢复默认校准");
  });
  server.onNotFound([]() {
    sendJson(404, "{\"error\":\"接口不存在\"}");
  });
}

void connectWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname("aquaph");
  apReady = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  Serial.printf("Wi-Fi AP: %s %s http://%s\n",
                WIFI_AP_SSID,
                apReady ? "ready" : "failed",
                WiFi.softAPIP().toString().c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptAt = millis();
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED || millis() - lastWifiAttemptAt < kWifiRetryMs) {
    return;
  }
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptAt = millis();
  Serial.println("Retrying Wi-Fi connection");
}

void setupOta() {
  ArduinoOTA.setHostname("aquaph");
  ArduinoOTA.setPort(kOtaPort);
  ArduinoOTA.setPassword(WIFI_OTA_PASSWORD);
  // Existing HTTP mDNS is managed below; OTA uploads use the explicit IP.
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    settings.autoEnabled = false;
    saveSettings();
    setRelay(false);
    Serial.println("OTA update started; relay forced OFF");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA update finished");
    otaInProgress = false;
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA error: %u\n", static_cast<unsigned>(error));
  });
  ArduinoOTA.begin();
  otaReady = true;
  Serial.printf("OTA ready: aquaph:%u\n", kOtaPort);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAquaPH controller booting");

  pinMode(kRelayPin, OUTPUT);
  digitalWrite(kRelayPin, kRelayActiveLow ? HIGH : LOW);
  relayChangedAt = millis() - kRelaySettleMs;

  loadSettings();
  Wire.begin(kSdaPin, kSclPin);
  adsReady = ads.begin(kAdsAddress, &Wire);
  if (adsReady) {
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_128SPS);
  }
  Serial.printf("ADS1115: %s\n", adsReady ? "ready" : "not found");

  connectWifi();
  setupOta();
  setupRoutes();
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  handleSerialCommands();
  server.handleClient();
  ArduinoOTA.handle();
  if (otaInProgress) {
    delay(2);
    return;
  }
  maintainWifi();
  samplePh();

  static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
  if (WiFi.status() != lastWifiStatus) {
    lastWifiStatus = WiFi.status();
    if (lastWifiStatus == WL_CONNECTED) {
      if (!mdnsReady) {
        mdnsReady = MDNS.begin("aquaph");
        if (mdnsReady) {
          MDNS.addService("http", "tcp", 80);
        }
      }
      Serial.printf("Wi-Fi connected: http://%s or http://aquaph.local\n",
                    WiFi.localIP().toString().c_str());
    }
  }
  delay(2);
}
