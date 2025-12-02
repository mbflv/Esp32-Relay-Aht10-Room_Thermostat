/* relay-with-sensors-variantA_ram_ntp_tooltip_final.ino
   For: ESP32 Dev Module
   - RAM history only
   - NTP clock with manual timezone offset (saved in Preferences)
   - History chart with hover tooltip (date/time, set.temp and temp)
   - Relay status shown on Sensors page
   - WiFi (AP + optional STA), Preferences for settings
   - Sensors polled via HTTP /json (temp/hum)
   - Scheduler + debounce + hysteresis
   - All settings persisted in Preferences
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#define RELAY_PIN 25
#define BUTTON_PIN 5
#define MAX_SENSORS 8
#define HISTORY_SLOTS 96            // number of samples per sensor in RAM
#define HISTORY_INTERVAL_MIN 30      // minutes between stored history samples (1 = every minute)
#define POLL_INTERVAL_MS 60000UL
#define DEFAULT_DEBOUNCE_SECS 1

WebServer server(80);
Preferences prefs;

// Auth default
String AUTH_USER = "admin";
String AUTH_PASS = "admin";

// WiFi defaults
String defaultAPPass = "12345678";
String apPass;
String savedSSID = "";
String savedPASS = "";
String staticIP = "";
String gatewayIP = "";
String subnetMask = "";

// Timezone offset in minutes (can be negative). Saved in prefs key "tz_min"
long tzOffsetMinutes = 0;

// Relay and button
bool relayState = false;
unsigned long pressStartTime = 0;
bool buttonPressed = false;

// Debounce
unsigned long lastDesiredRelayChange = 0;
unsigned long debounceSeconds = DEFAULT_DEBOUNCE_SECS * 1000UL;
bool desiredRelayState = false;

// Scheduler: 4 programs
struct Program {
  int startHour;
  int stopHour;
  float setTemp;
  bool enabled;
};
Program programs[4];

// Sensor struct
struct Sensor {
  String name;
  String ip;
  float tempOffset;
  float humOffset;
  float setTemp;
  float hysteresis;
  float lastTemp;
  float lastHum;
  bool isMain;
  bool enabled;
};
Sensor sensors[MAX_SENSORS];

// History entry (RAM)
struct HistEntry {
  uint32_t ts;
  float temp;
  float hum;
  float setTemp;
};
// In-RAM circular buffers
HistEntry histBuf[MAX_SENSORS][HISTORY_SLOTS];
uint16_t histIdx[MAX_SENSORS]; // next write position (0..HISTORY_SLOTS-1)

// Helpers
String f1(float v){ return String(v,1); }
void relayOn(){ pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW); relayState=true; }
void relayOff(){ pinMode(RELAY_PIN, INPUT); digitalWrite(RELAY_PIN, LOW); relayState=false; } // open-drain style

// Preferences key helpers
String prefKeySensor(int idx, const char* k){
  char buf[32]; snprintf(buf, sizeof(buf), "s%d_%s", idx, k); return String(buf);
}
String prefKeyProg(int p, const char* k){ char buf[32]; snprintf(buf,sizeof(buf),"prog%d_%s",p,k); return String(buf); }

// Factory reset
void factoryReset(){
  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  prefs.begin("sensors", false); prefs.clear(); prefs.end();
  prefs.begin("prog", false); prefs.clear(); prefs.end();
  delay(500);
  ESP.restart();
}

// Load / Save sensors & programs & prefs
void loadSensors(){
  prefs.begin("sensors", true);
  for(int i=0;i<MAX_SENSORS;i++){
    sensors[i].ip = prefs.getString(prefKeySensor(i,"ip").c_str(), "");
    sensors[i].name = prefs.getString(prefKeySensor(i,"name").c_str(), String("Sensor " + String(i+1)));
    sensors[i].tempOffset = prefs.getFloat(prefKeySensor(i,"toff").c_str(), 0.0f);
    sensors[i].humOffset = prefs.getFloat(prefKeySensor(i,"hoff").c_str(), 0.0f);
    sensors[i].setTemp = prefs.getFloat(prefKeySensor(i,"set").c_str(), 25.0f);
    sensors[i].hysteresis = prefs.getFloat(prefKeySensor(i,"hys").c_str(), 1.0f);
    sensors[i].isMain = prefs.getBool(prefKeySensor(i,"main").c_str(), false);
    sensors[i].lastTemp = NAN;
    sensors[i].lastHum = NAN;
    sensors[i].enabled = (sensors[i].ip.length() > 0);
  }
  prefs.end();
}

void saveSensors(){
  prefs.begin("sensors", false);
  for(int i=0;i<MAX_SENSORS;i++){
    prefs.putString(prefKeySensor(i,"ip").c_str(), sensors[i].ip);
    prefs.putString(prefKeySensor(i,"name").c_str(), sensors[i].name);
    prefs.putFloat(prefKeySensor(i,"toff").c_str(), sensors[i].tempOffset);
    prefs.putFloat(prefKeySensor(i,"hoff").c_str(), sensors[i].humOffset);
    prefs.putFloat(prefKeySensor(i,"set").c_str(), sensors[i].setTemp);
    prefs.putFloat(prefKeySensor(i,"hys").c_str(), sensors[i].hysteresis);
    prefs.putBool(prefKeySensor(i,"main").c_str(), sensors[i].isMain);
  }
  prefs.end();
}

void loadPrograms(){
  prefs.begin("prog", true);
  for(int p=0;p<4;p++){
    programs[p].startHour = prefs.getInt(prefKeyProg(p,"start").c_str(), 0);
    programs[p].stopHour  = prefs.getInt(prefKeyProg(p,"stop").c_str(), 0);
    programs[p].setTemp   = prefs.getFloat(prefKeyProg(p,"temp").c_str(), 20.0);
    programs[p].enabled   = prefs.getBool(prefKeyProg(p,"en").c_str(), false);
  }
  prefs.end();
}

void savePrograms(){
  prefs.begin("prog", false);
  for(int p=0;p<4;p++){
    prefs.putInt(prefKeyProg(p,"start").c_str(), programs[p].startHour);
    prefs.putInt(prefKeyProg(p,"stop").c_str(), programs[p].stopHour);
    prefs.putFloat(prefKeyProg(p,"temp").c_str(), programs[p].setTemp);
    prefs.putBool(prefKeyProg(p,"en").c_str(), programs[p].enabled);
  }
  prefs.end();
}

void loadWifiAuth(){
  prefs.begin("wifi", true);
  savedSSID = prefs.getString("ssid", "");
  savedPASS = prefs.getString("pass", "");
  apPass = prefs.getString("appass", defaultAPPass);
  staticIP = prefs.getString("ip", "");
  gatewayIP = prefs.getString("gw", "");
  subnetMask = prefs.getString("subnet", "");
  AUTH_USER = prefs.getString("auth_user", "admin");
  AUTH_PASS = prefs.getString("auth_pass", "admin");
  uint32_t db = prefs.getUInt("deb_ms", DEFAULT_DEBOUNCE_SECS * 1000UL);
  debounceSeconds = db;
  tzOffsetMinutes = prefs.getLong("tz_min", 0); // load timezone offset minutes
  prefs.end();
}
void saveWifiAuth(){
  prefs.begin("wifi", false);
  prefs.putString("ssid", savedSSID);
  prefs.putString("pass", savedPASS);
  prefs.putString("appass", apPass);
  prefs.putString("ip", staticIP);
  prefs.putString("gw", gatewayIP);
  prefs.putString("subnet", subnetMask);
  prefs.putString("auth_user", AUTH_USER);
  prefs.putString("auth_pass", AUTH_PASS);
  prefs.putUInt("deb_ms", (uint32_t)debounceSeconds);
  prefs.putLong("tz_min", tzOffsetMinutes);
  prefs.end();
}

// ===== In-RAM history helpers =====
void initHistoryRam(){
  for(int i=0;i<MAX_SENSORS;i++){
    histIdx[i] = 0;
    for(int s=0;s<HISTORY_SLOTS;s++){
      histBuf[i][s].ts = 0;
      histBuf[i][s].temp = NAN;
      histBuf[i][s].hum = NAN;
      histBuf[i][s].setTemp = 0;
    }
  }
}

void appendHistoryRam(int si, uint32_t ts, float temp, float hum, float setT){
  if(si<0 || si>=MAX_SENSORS) return;
  uint16_t idx = histIdx[si] % HISTORY_SLOTS;
  histBuf[si][idx].ts = ts;
  histBuf[si][idx].temp = temp;
  histBuf[si][idx].hum = hum;
  histBuf[si][idx].setTemp = setT;
  histIdx[si] = (idx + 1) % HISTORY_SLOTS;
}

// Build JSON history oldest->newest into provided JsonArray
void jsonHistoryForSensor(int si, JsonArray &arr){
  if(si<0 || si>=MAX_SENSORS) return;
  uint16_t w = histIdx[si] % HISTORY_SLOTS; // next write pos; oldest at w
  for(int i=0;i<HISTORY_SLOTS;i++){
    int pos = (w + i) % HISTORY_SLOTS;
    if(histBuf[si][pos].ts == 0) continue;
    JsonObject o = arr.createNestedObject();
    o["ts"] = histBuf[si][pos].ts - tzOffsetMinutes*60;

    //o["ts"] = histBuf[si][pos].ts;
    if(isnan(histBuf[si][pos].temp)) o["temp"] = nullptr; else o["temp"] = histBuf[si][pos].temp;
    if(isnan(histBuf[si][pos].hum))  o["hum"]  = nullptr; else o["hum"]  = histBuf[si][pos].hum;
    o["set"] = histBuf[si][pos].setTemp;
  }
}

// ===== Poll single sensor by IP (expects JSON with fields "temp" and "hum") =====
bool pollSensor(int idx){
  if(idx<0 || idx>=MAX_SENSORS) return false;
  if(!sensors[idx].enabled || sensors[idx].ip.length()==0) return false;

  HTTPClient http;
  String url = "http://" + sensors[idx].ip + "/json";
  http.setTimeout(3000);
  if(!http.begin(url)) {
    http.end();
    return false;
  }
  int code = http.GET();
  if(code==200){
    String payload = http.getString();
    http.end();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if(!err){
      float t = NAN, h = NAN;
      if(doc.containsKey("temp")) t = doc["temp"].as<float>();
      else if(doc.containsKey("temperature")) t = doc["temperature"].as<float>();
      if(doc.containsKey("hum")) h = doc["hum"].as<float>();
      else if(doc.containsKey("humidity")) h = doc["humidity"].as<float>();
      if(!isnan(t)) sensors[idx].lastTemp = t + sensors[idx].tempOffset;
      if(!isnan(h)) sensors[idx].lastHum = h + sensors[idx].humOffset;
      return true;
    }
  } else {
    http.end();
  }
  return false;
}

// ===== JSON endpoints =====
void handleSensorsJson(){
  DynamicJsonDocument doc(2048);
  doc["relay"] = relayState ? "ON" : "OFF";
  JsonArray arr = doc.createNestedArray("sensors");
  for(int i=0;i<MAX_SENSORS;i++){
    if(!sensors[i].enabled) continue;
    JsonObject s = arr.createNestedObject();
    s["index"] = i;
    s["name"] = sensors[i].name;
    s["ip"] = sensors[i].ip;
    if(isnan(sensors[i].lastTemp)) s["temp"] = nullptr; else s["temp"] = sensors[i].lastTemp;
    if(isnan(sensors[i].lastHum))  s["hum"] = nullptr;  else s["hum"] = sensors[i].lastHum;
    s["toff"] = sensors[i].tempOffset;
    s["hoff"] = sensors[i].humOffset;
    s["set"]  = sensors[i].setTemp;
    s["hys"]  = sensors[i].hysteresis;
    s["main"] = sensors[i].isMain;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// History JSON
void handleHistoryJson(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  int si = server.arg("i").toInt();
  if(si<0 || si>=MAX_SENSORS){ server.send(400,"text/plain","Bad index"); return; }
  DynamicJsonDocument doc(4096 + HISTORY_SLOTS*48);
  doc["sensor"] = sensors[si].name;
  doc["index"] = si;
  JsonArray arr = doc.createNestedArray("history");
  jsonHistoryForSensor(si, arr);
  String out; serializeJson(doc, out);
  server.send(200,"application/json", out);
}

// Time endpoint (returns adjusted epoch and ISO string)
String isoFromEpoch(time_t t){
  struct tm tmnow;
  gmtime_r(&t, &tmnow);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           tmnow.tm_year+1900, tmnow.tm_mon+1, tmnow.tm_mday,
           tmnow.tm_hour, tmnow.tm_min, tmnow.tm_sec);
  return String(buf);
}
void handleTimeJson(){
  time_t nowt = time(NULL);
  if(nowt <= 0) {
    server.send(200,"application/json","{\"epoch\":0,\"iso\":\"N/A\"}");
    return;
  }
  long adj = nowt + tzOffsetMinutes * 60;
  String out = "{\"epoch\":" + String((uint32_t)adj) + ",\"iso\":\"" + isoFromEpoch((time_t)adj) + "\"}";
  server.send(200,"application/json", out);
}

// ===== Web UI root (inline HTML + JS) =====
void handleRoot(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();

  String macStr = WiFi.macAddress();
  String ipStr = (WiFi.status()==WL_CONNECTED) ? WiFi.localIP().toString() : "N/A";
  long rssi = WiFi.RSSI();
  String rssiStr = (WiFi.status()==WL_CONNECTED) ? String(rssi)+" dBm" : "N/A";

  // Build sensors HTML cards, include relay status at top
  String sensorsHtml = "";
  sensorsHtml += "<div class='card'><h3>Relay status</h3><p id='relayStateMain'>--</p></div>";
  for(int i=0;i<MAX_SENSORS;i++){
    if(!sensors[i].enabled) continue;
    sensorsHtml += "<div class='card' id='sensorCard"+String(i)+"'>";
    sensorsHtml += "<h4><input class='sname' id='name"+String(i)+"' value='"+sensors[i].name+"' onblur='saveName("+String(i)+")'></h4>";
    sensorsHtml += "<p><b>IP:</b> "+sensors[i].ip+"</p>";
    sensorsHtml += "<p>Temp: <span id='temp"+String(i)+"'>"+ (isnan(sensors[i].lastTemp)?"--":f1(sensors[i].lastTemp)) +"</span>&deg;C  Hum: <span id='hum"+String(i)+"'>"+ (isnan(sensors[i].lastHum)?"--":f1(sensors[i].lastHum)) +"</span>%</p>";
    sensorsHtml += "<p>Set: <button class='small' onclick='adjSet("+String(i)+",-0.1)'>-</button> <span id='set"+String(i)+"'>"+f1(sensors[i].setTemp)+"</span> <button class='small' onclick='adjSet("+String(i)+",0.1)'>+</button></p>";
    sensorsHtml += "<p>Hyst: <button class='small' onclick='adjHys("+String(i)+",-0.1)'>-</button> <span id='hys"+String(i)+"'>"+f1(sensors[i].hysteresis)+"</span> <button class='small' onclick='adjHys("+String(i)+",0.1)'>+</button></p>";
    sensorsHtml += "<p>Offset T: <button class='small' onclick='adjOff("+String(i)+",-0.1)'>-</button> <span id='offt"+String(i)+"'>"+f1(sensors[i].tempOffset)+"</span> <button class='small' onclick='adjOff("+String(i)+",0.1)'>+</button></p>";
    sensorsHtml += "<p>Offset H: <button class='small' onclick='adjOffH("+String(i)+",-0.1)'>-</button> <span id='offh"+String(i)+"'>"+f1(sensors[i].humOffset)+"</span> <button class='small' onclick='adjOffH("+String(i)+",0.1)'>+</button></p>";
    sensorsHtml += "<p><label><input type='radio' name='mainSensor' onchange='setMain("+String(i)+")' "+(sensors[i].isMain?"checked":"")+"> Main sensor</label></p>";
    sensorsHtml += "<p><button class='del' onclick='removeSensor("+String(i)+")'>Remove</button> <button class='btn' onclick='viewHistory("+String(i)+")'>History</button></p>";
    sensorsHtml += "</div>";
  }

  // Add-sensor UI
  bool canAdd = false;
  for(int i=0;i<MAX_SENSORS;i++){ if(!sensors[i].enabled){ canAdd=true; break; } }
  String addHtml = "";
  if(canAdd){
    addHtml += "<div class='card'><h4>Add Sensor</h4>";
    addHtml += "<label>IP</label><input id='add_ip' class='tiny' placeholder='192.168.1.50'><br>";
    addHtml += "<label>Name</label><input id='add_name' class='tiny' placeholder='Livingroom'><br>";
    addHtml += "<button class='btn' onclick='addSensor()'>Add sensor</button></div>";
  } else {
    addHtml += "<div class='card'><p>Max sensors reached ("+String(MAX_SENSORS)+")</p></div>";
  }

  // Programs UI
  String progHtml = "<div class='card'><h4>Scheduler (4 programs)</h4>";
  for(int p=0;p<4;p++){
    progHtml += "<div style='margin-bottom:6px'>";
    progHtml += "Program " + String(p+1) + ": <input class='tiny' id='p"+String(p)+"_start' value='"+String(programs[p].startHour)+"' type='number' min='0' max='23'> - ";
    progHtml += "<input class='tiny' id='p"+String(p)+"_stop' value='"+String(programs[p].stopHour)+"' type='number' min='0' max='23'> ";
    progHtml += "Set: <input class='tiny' id='p"+String(p)+"_temp' value='"+f1(programs[p].setTemp)+"' step='0.1' type='number'> ";
    progHtml += "<label><input type='checkbox' id='p"+String(p)+"_en' "+(programs[p].enabled?"checked":"")+"> Enabled</label>";
    progHtml += "</div>";
  }
  progHtml += "<button class='btn' onclick='savePrograms()'>Save Programs</button></div>";

  // Debounce UI
  String debounceHtml = "<div class='card'><h4>Debounce</h4>";
  debounceHtml += "Debounce (seconds): <input class='tiny' id='deb' value='"+String(debounceSeconds/1000)+"' type='number' min='0'> <button class='btn' onclick='saveDebounce()'>Save</button></div>";

  // Timezone UI
  String tzHtml = "<div class='card'><h4>Ceas NTP & Offset</h4>";
  tzHtml += "<p>Server time (adjusted): <span id='srv_time'>...</span></p>";
  tzHtml += "Offset (hours, can be negative): <input class='tiny' id='tz_hours' value='"+String((float)tzOffsetMinutes/60.0,2)+"' step='0.25' type='number'> ";
  tzHtml += "<button class='btn' onclick='saveTZ()'>Save offset</button></div>";

  // Compose page using String(...) wrappers to avoid operator ambiguity
  String page = String(R"rawliteral(
<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Relay + Sensors (Variant A - RAM history)</title>
<style>
:root{--bg:#111;--card:#1b1b1b;--accent:#2979ff;--muted:#bdbdbd;--white:#fff}
body{background:var(--bg);color:var(--white);margin:0;font-family:Arial,Helvetica,sans-serif}
.header{padding:10px 14px;background:#161616;display:flex;gap:8px;flex-wrap:wrap}
.tab{padding:8px 12px;color:var(--muted);cursor:pointer}
.tab.active{color:var(--white);background:#222;border-radius:6px}
.container{padding:12px}
.card{background:var(--card);padding:10px;border-radius:8px;margin-bottom:10px}
.btn{background:var(--accent);padding:8px;border:none;color:#fff;border-radius:6px;cursor:pointer}
.small{width:26px;height:26px;border-radius:6px;border:none;background:#333;color:#fff;cursor:pointer}
.del{background:#c62828;padding:6px;border:none;color:#fff;border-radius:6px;cursor:pointer}
.input{width:160px;padding:6px;border-radius:6px;border:1px solid #333;background:#0f0f0f;color:#fff}
.tiny{width:80px;padding:6px;border-radius:6px;border:1px solid #333;background:#0f0f0f;color:#fff}
.sname{width:180px;padding:6px;border-radius:6px;border:1px solid #333;background:#0f0f0f;color:#fff}
.sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:8px}
.info{font-size:13px;color:var(--muted)}
.chart-wrap{width:100%;height:320px;background:#0b0b0b;border-radius:8px;padding:8px;position:relative}
#tooltip{position:absolute;pointer-events:none;background:#222;padding:6px;border-radius:6px;border:1px solid #444;display:none;font-size:12px}
</style></head><body>
<div class='header'>
  <div class='tab active' id='tabControl' onclick="showTab('control')">Control</div>
  <div class='tab' id='tabSensors' onclick="showTab('sensors')">Sensors</div>
  <div class='tab' id='tabWifi' onclick="showTab('wifi')">Wi-Fi</div>
  <div class='tab' id='tabAuth' onclick="showTab('auth')">Web Auth</div>
  <div class='tab' id='tabHistory' onclick="showTab('history')">History</div>
  <div class='tab' id='tabSched' onclick="showTab('sched')">Scheduler</div>
</div>
<div class='container'>

<div id='controlTab'>
  <div class='card'>
    <h3>Control</h3>
    <p class='info'>Advanced relay control is available via Sensors page and automatic rules (scheduler/hysteresis).</p>

    <canvas id='tempGauge' width='300' height='150'></canvas>

    <div style='margin-top:10px;'>
      <button class='small' onclick='adjustSet(-0.1)'>-</button>
      <span id='setTempVal'>--</span> &deg;C
      <button class='small' onclick='adjustSet(0.1)'>+</button>
    </div>

    <p>Current Temp: <span id='currentTempVal'>--</span> &deg;C</p>
    <p>Humidity: <span id='currentHumVal'>--</span> %</p>
    <p>Status: <span id='heatingStatus'>--</span></p>
    <p class='info'>----------------------------------</p>
    <p class='info'>Developed by Flaviu.Inspired by your needs.</p>
  </div>
</div>

<div id='sensorsTab' style='display:none'>
  <div class='card'>
    <h3>Sensors</h3>
    <div class='sensor-grid'>
)rawliteral") + sensorsHtml + String(R"rawliteral(
    </div>
  </div>
  )rawliteral") + addHtml + String(R"rawliteral(
</div>

<div id='wifiTab' style='display:none'>
  <div class='card'>
    <h3>Wi-Fi & Network</h3>
    <p class='info'><b>SSID:</b> )rawliteral") + savedSSID + String(R"rawliteral( &nbsp; <b>MAC:</b> )rawliteral") + macStr + String(R"rawliteral( &nbsp; <b>IP:</b> )rawliteral") + ipStr + String(R"rawliteral( &nbsp; <b>RSSI:</b> )rawliteral") + rssiStr + String(R"rawliteral(</p>
    <form onsubmit="saveWifi();return false;">
      <label>SSID</label><input id='ssid' class='input' value=')rawliteral") + savedSSID + String(R"rawliteral('><br>
      <label>Password</label><input id='pass' class='input' value=')rawliteral") + savedPASS + String(R"rawliteral('><br>
      <label>AP Password</label><input id='appass' class='input' value=')rawliteral") + apPass + String(R"rawliteral('><br>
      <label>Static IP</label><input id='sip' class='input' value=')rawliteral") + staticIP + String(R"rawliteral(' placeholder='optional'><br>
      <label>Gateway</label><input id='gw' class='input' value=')rawliteral") + gatewayIP + String(R"rawliteral(' placeholder='optional'><br>
      <label>Subnet</label><input id='sm' class='input' value=')rawliteral") + subnetMask + String(R"rawliteral(' placeholder='optional'><br>
      <button class='btn' type='submit'>Save & Reboot</button>
    </form>
  </div>
  )rawliteral") + tzHtml + String(R"rawliteral(
</div>

<div id='authTab' style='display:none'>
  <div class='card'>
    <h3>Web Auth</h3>
    <form onsubmit="saveAuth();return false;">
      <label>Username</label><input id='webuser' class='input' value=')rawliteral") + AUTH_USER + String(R"rawliteral('><br>
      <label>Password</label><input id='webpass' class='input' value=')rawliteral") + AUTH_PASS + String(R"rawliteral('><br>
      <button class='btn' type='submit'>Save & Reboot</button>
    </form>
  </div>
</div>

<div id='historyTab' style='display:none'>
  <div class='card'>
    <h3>History Viewer</h3>
    <label>Sensor index</label><input id='hist_idx' class='tiny' value='0' type='number' min='0' max='7'>
    <button class='btn' onclick='showHistory()'>Show</button>
    <div class='chart-wrap'>
      <canvas id='chart' width='900' height='300'></canvas>
      <div id='tooltip'></div>
    </div>
  </div>
</div>

<div id='sched' style='display:none'>
  )rawliteral") + progHtml + debounceHtml + String(R"rawliteral(
</div>

</div>

<script>
// UI logic
function showTab(t){
  ['control','sensors','wifi','auth','history','sched'].forEach(x=>{
    let elTab = document.getElementById(x+'Tab');
    if(elTab) elTab.classList.toggle('active', x==t);
  });
  document.getElementById('controlTab').style.display=(t=='control'?'block':'none');
  document.getElementById('sensorsTab').style.display=(t=='sensors'?'block':'none');
  document.getElementById('wifiTab').style.display=(t=='wifi'?'block':'none');
  document.getElementById('authTab').style.display=(t=='auth'?'block':'none');
  document.getElementById('historyTab').style.display=(t=='history'?'block':'none');
  document.getElementById('sched').style.display=(t=='sched'?'block':'none');
}

// network helpers
async function fetchJSON(path){ try{ let r = await fetch(path); if(!r.ok) return null; return await r.json(); }catch(e){ return null; } }
async function fetchText(path,opts){ try{ let r = await fetch(path,opts); return await r.text(); }catch(e){ return null; } }

async function updateUI(){
  let data = await fetchJSON('/sensors_json');
  if(!data) return;
  // update relay status displayed on Sensors page
  if(document.getElementById('relayStateMain')) document.getElementById('relayStateMain').innerText = 'Relay: ' + data.relay;
  if(data.sensors){
    for(let s of data.sensors){
      let i = s.index;
      let tEl = document.getElementById('temp'+i);
      let hEl = document.getElementById('hum'+i);
      let setEl = document.getElementById('set'+i);
      let hysEl = document.getElementById('hys'+i);
      let offtEl = document.getElementById('offt'+i);
      let offhEl = document.getElementById('offh'+i);
      if(tEl) tEl.innerText = (s.temp===null?'--':parseFloat(s.temp).toFixed(1));
      if(hEl) hEl.innerText = (s.hum===null?'--':parseFloat(s.hum).toFixed(1));
      if(setEl) setEl.innerText = parseFloat(s.set).toFixed(1);
      if(hysEl) hysEl.innerText = parseFloat(s.hys).toFixed(1);
      if(offtEl) offtEl.innerText = parseFloat(s.toff).toFixed(1);
      if(offhEl) offhEl.innerText = parseFloat(s.hoff).toFixed(1);
    }
  }
}

// Actions (note: using GET endpoints for simple interaction)
async function addSensor(){
  let ip = document.getElementById('add_ip').value.trim();
  let name = document.getElementById('add_name').value.trim();
  if(!ip){ alert('IP required'); return; }
  await fetch('/addSensor?ip='+encodeURIComponent(ip)+'&name='+encodeURIComponent(name));
  location.reload();
}
async function removeSensor(i){ if(!confirm('Remove sensor?')) return; await fetch('/removeSensor?i='+i); location.reload(); }
async function adjSet(i,delta){ await fetch('/adj?i='+i+'&type=set&v='+delta); updateUI(); }
async function adjHys(i,delta){ await fetch('/adj?i='+i+'&type=hys&v='+delta); updateUI(); }
async function adjOff(i,delta){ await fetch('/adj?i='+i+'&type=offt&v='+delta); updateUI(); }
async function adjOffH(i,delta){ await fetch('/adj?i='+i+'&type=offh&v='+delta); updateUI(); }
async function setMain(i){ await fetch('/setMain?i='+i); updateUI(); }
async function saveName(i){ let v=document.getElementById('name'+i).value; await fetch('/setName?i='+i+'&name='+encodeURIComponent(v)); updateUI(); }

// WiFi & Auth
async function saveWifi(){ let s=document.getElementById('ssid').value; let p=document.getElementById('pass').value; let ap=document.getElementById('appass').value; let sip=document.getElementById('sip').value; let gw=document.getElementById('gw').value; let sm=document.getElementById('sm').value; let body=new URLSearchParams(); body.append('s',s); body.append('p',p); body.append('ap',ap); body.append('ip',sip); body.append('gw',gw); body.append('sm',sm); await fetch('/savewifi',{method:'POST',body:body}); alert('Saved, rebooting'); }
async function saveAuth(){ let u=document.getElementById('webuser').value; let p=document.getElementById('webpass').value; let body=new URLSearchParams(); body.append('u',u); body.append('p',p); await fetch('/saveauth',{method:'POST',body:body}); alert('Saved, rebooting'); }

// Scheduler
function savePrograms(){
  let body = new URLSearchParams();
  for(let p=0;p<4;p++){
    body.append('p'+p+'_start', document.getElementById('p'+p+'_start').value);
    body.append('p'+p+'_stop', document.getElementById('p'+p+'_stop').value);
    body.append('p'+p+'_temp', document.getElementById('p'+p+'_temp').value);
    body.append('p'+p+'_en', document.getElementById('p'+p+'_en').checked ? '1':'0');
  }
  fetch('/savePrograms',{method:'POST', body: body}).then(()=>{ alert('Programs saved'); });
}
function saveDebounce(){
  let v = parseInt(document.getElementById('deb').value) || 0;
  let body = new URLSearchParams(); body.append('deb', String(v));
  fetch('/saveDebounce',{method:'POST',body:body}).then(()=>{ alert('Saved'); });
}

// Timezone save
function saveTZ(){
  let h = parseFloat(document.getElementById('tz_hours').value) || 0;
  let minutes = Math.round(h * 60);
  let body = new URLSearchParams(); body.append('tz', String(minutes));
  fetch('/settz',{method:'POST', body: body}).then(()=>{ alert('Saved TZ offset'); fetchTime(); });
}
async function fetchTime(){
  let data = await fetchJSON('/time.json');
  if(data){
    document.getElementById('srv_time').innerText = data.iso + " (epoch:" + data.epoch + ")";
  }
}

// History viewer with tooltip
let chartData = [];
function viewHistory(i){ showTab('history'); document.getElementById('hist_idx').value = i; showHistory(); }

async function showHistory(){
  let i = parseInt(document.getElementById('hist_idx').value);
  if(isNaN(i) || i<0) return;
  // history.json is protected; browser will prompt for auth
  let data = await fetchJSON('/history.json?i='+i);
  if(!data) return alert('No data or auth required');
  chartData = data.history || [];
  drawChartWithTooltip(chartData);
}

function drawChartWithTooltip(hist){
  const canvas = document.getElementById('chart');
  const tooltip = document.getElementById('tooltip');
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0,0,canvas.width,canvas.height);
  if(!hist || hist.length==0){ ctx.fillStyle='#666'; ctx.fillText('No history', 10,20); tooltip.style.display='none'; return; }

  // prepare arrays
  const temps = hist.map(h=> h.temp===null ? null : parseFloat(h.temp));
  const sets = hist.map(h=> parseFloat(h.set));
  // find min/max
  let minV = 9999, maxV = -9999;
  for(let v of temps){ if(v!==null){ minV=Math.min(minV,v); maxV=Math.max(maxV,v);} }
  for(let v of sets){ minV=Math.min(minV,v); maxV=Math.max(maxV,v); }
  if(minV==9999){ minV=0; maxV=50; }
  const pad = (maxV-minV)*0.1 || 1;
  minV -= pad; maxV += pad;

  const left = 10, right = canvas.width-10;
  const top = 10, bottom = canvas.height-20;
  const w = right-left;
  const n = hist.length;
  function xFor(i){ return left + (n==1? w/2 : (i/(n-1))*w); }
  function yFor(v){ return bottom - ((v - minV)/(maxV-minV))*(bottom-top); }

  // grid
  ctx.strokeStyle = '#222'; ctx.lineWidth = 1;
  for(let g=0; g<=5; g++){ let y = top + g*(bottom-top)/5; ctx.beginPath(); ctx.moveTo(left,y); ctx.lineTo(right,y); ctx.stroke(); }

  // draw set line
  ctx.beginPath(); ctx.strokeStyle = '#ffa500'; ctx.lineWidth = 2;
  for(let i=0;i<n;i++){ let x=xFor(i), y=yFor(sets[i]); if(i==0) ctx.moveTo(x,y); else ctx.lineTo(x,y); }
  ctx.stroke();

  // draw temp line
  ctx.beginPath(); ctx.strokeStyle = '#00bfff'; ctx.lineWidth = 2;
  let first = true;
  for(let i=0;i<n;i++){ let t = temps[i]; if(t===null) { first = true; continue; } let x=xFor(i), y=yFor(t); if(first){ ctx.moveTo(x,y); first=false; } else ctx.lineTo(x,y); }
  ctx.stroke();

  // points
  for(let i=0;i<n;i++){
    let v = temps[i]; if(v===null) continue;
    let x = xFor(i), y=yFor(v);
    ctx.beginPath(); ctx.fillStyle='#00bfff'; ctx.arc(x,y,3,0,Math.PI*2); ctx.fill();
  }

  // attach mouse events for tooltip
  canvas.onmousemove = function(e){
    const rect = canvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;
    // find nearest point by x (hist points evenly spaced)
    let closestIndex = -1;
    let closestDist = 1e9;
    for(let i=0;i<n;i++){
      let x = xFor(i);
      let d = Math.abs(mx - x);
      if(d < closestDist){ closestDist = d; closestIndex = i; }
    }
    // threshold (10px)
    if(closestDist < 10){
      const entry = hist[closestIndex];
      const displayTs = new Date(entry.ts * 1000);
      const iso = displayTs.getFullYear() + '-' + String(displayTs.getMonth()+1).padStart(2,'0') + '-' + String(displayTs.getDate()).padStart(2,'0')
                  + ' ' + String(displayTs.getHours()).padStart(2,'0') + ':' + String(displayTs.getMinutes()).padStart(2,'0') + ':' + String(displayTs.getSeconds()).padStart(2,'0');
      const tempText = (entry.temp===null) ? 'N/A' : parseFloat(entry.temp).toFixed(1) + "&degC";
      const setText = (entry.set===null) ? 'N/A' : parseFloat(entry.set).toFixed(1) + "&degC";
      tooltip.style.display = 'block';
      tooltip.style.left = (xFor(closestIndex)+10) + 'px';
      tooltip.style.top = (yFor(entry.temp||entry.set) - 40) + 'px';
      tooltip.innerHTML = "<b>"+iso+"</b><br>Temp: "+tempText+"<br>Set: "+setText;
    } else {
      tooltip.style.display = 'none';
    }
  };
  canvas.onmouseleave = function(){ document.getElementById('tooltip').style.display='none'; };
}

// init
setInterval(updateUI,3000);
updateUI();
fetchTime();
setInterval(fetchTime,5000);

//gauge
let currentTemp = null;
let setTemp = null;
let relayOnState = false;

function drawGauge(){
  if (setTemp === null || currentTemp === null || isNaN(setTemp) || isNaN(currentTemp)) {
    const canvas = document.getElementById('tempGauge');
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    return;
}
  const canvas = document.getElementById('tempGauge');
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0,0,canvas.width,canvas.height);

  if(setTemp === null || currentTemp === null) return;

  // Gauge background
  ctx.fillStyle='#222';
  ctx.fillRect(0,0,canvas.width,canvas.height);

  const centerX = canvas.width/2;
  const centerY = canvas.height;
  const radius = Math.min(canvas.width/2, canvas.height-20);

  // Draw gauge arc
  ctx.beginPath();
  ctx.arc(centerX, centerY, radius, Math.PI, 0, false);
  ctx.lineWidth = 20;
  ctx.strokeStyle = '#444';
  ctx.stroke();

  // Draw current temp
  const tempRatio = Math.min(Math.max(currentTemp/50, 0),1);
  const tempAngle = Math.PI + tempRatio*Math.PI;
  ctx.beginPath();
  ctx.arc(centerX, centerY, radius, Math.PI, tempAngle, false);
  ctx.lineWidth = 20;
  ctx.strokeStyle = '#00bfff';
  ctx.stroke();

  // Draw set temp marker
  const setRatio = Math.min(Math.max(setTemp/50,0),1);
  const setAngle = Math.PI + setRatio*Math.PI;
  ctx.beginPath();
  ctx.moveTo(centerX + (radius+10)*Math.cos(setAngle), centerY + (radius+10)*Math.sin(setAngle));
  ctx.lineTo(centerX + (radius+30)*Math.cos(setAngle), centerY + (radius+30)*Math.sin(setAngle));
  ctx.lineWidth = 4;
  ctx.strokeStyle = '#ffa500';
  ctx.stroke();
}

async function updateControlUI(){
  const data = await fetchJSON('/sensors_json');
  if(!data || !data.sensors) return;

  let main = data.sensors.find(s => s.main);
  if (!main || main.temp === null || main.hum === null) {
    document.getElementById('currentTempVal').innerText = "sensor offline";
    document.getElementById('currentHumVal').innerText  = "sensor offline";
    document.getElementById('setTempVal').innerText     = "--";
    document.getElementById('heatingStatus').innerText  = "--";

    // clear gauge
    const canvas = document.getElementById('tempGauge');
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0,0,canvas.width,canvas.height);
    return;
}

  currentTemp = main.temp;
  setTemp = main.set;

  relayOnState = (data.relay === "ON");

  document.getElementById('heatingStatus').innerText = relayOnState ? "Heating ON" : "Heating OFF";
  document.getElementById('setTempVal').innerText = setTemp.toFixed(1);
  document.getElementById('currentTempVal').innerText = currentTemp.toFixed(1);
  document.getElementById('currentHumVal').innerText = main.hum !== null ? main.hum.toFixed(1) : '--';
  drawGauge();
}

async function adjustSet(delta){
  if(setTemp === null) return;

  let newSet = setTemp + delta;
  if(newSet < 0) newSet = 0;
  if(newSet > 50) newSet = 50;
  
  // Actualizează imediat UI
  setTemp = newSet;
  document.getElementById('setTempVal').innerText = setTemp.toFixed(1);
  drawGauge();

  // Trimite comanda către server
  let main = await fetchJSON('/sensors_json');
  if(!main || !main.sensors) return;
  let idx = main.sensors.find(s => s.main)?.index;
  if(idx === undefined) return;
  await fetch(`/adj?i=${idx}&type=set&v=${delta}`);
}

// Refresh la fiecare 3s
setInterval(updateControlUI, 3000);
updateControlUI();


</script>
</body></html>
)rawliteral");

  server.send(200, "text/html", page);
}

// ===== Web handlers =====
void handleOn(){ if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication(); desiredRelayState = true; lastDesiredRelayChange = millis(); server.send(200,"text/plain","ON"); }
void handleOff(){ if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication(); desiredRelayState = false; lastDesiredRelayChange = millis(); server.send(200,"text/plain","OFF"); }
void handleState(){ if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication(); server.send(200,"text/plain", relayState ? "ON" : "OFF"); }
void handleSensorsJson_prot(){ if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication(); handleSensorsJson(); }

void handleAddSensor(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  String ip = server.arg("ip");
  String name = server.arg("name");
  if(ip.length()==0){ server.send(400,"text/plain","IP required"); return; }
  for(int i=0;i<MAX_SENSORS;i++){
    if(!sensors[i].enabled){
      sensors[i].ip = ip; sensors[i].name = name.length()?name:("Sensor "+String(i+1));
      sensors[i].tempOffset = 0; sensors[i].humOffset = 0; sensors[i].setTemp = 25.0; sensors[i].hysteresis = 1.0;
      sensors[i].lastTemp = NAN; sensors[i].lastHum = NAN; sensors[i].isMain = false; sensors[i].enabled = true;
      saveSensors();
      // init RAM history slot
      for(int s=0;s<HISTORY_SLOTS;s++){ histBuf[i][s].ts=0; histBuf[i][s].temp=NAN; histBuf[i][s].hum=NAN; histBuf[i][s].setTemp = sensors[i].setTemp; }
      histIdx[i]=0;
      server.send(200,"text/plain","OK");
      return;
    }
  }
  server.send(503,"text/plain","No slot");
}

void handleRemoveSensor(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  int i = server.arg("i").toInt();
  if(i<0 || i>=MAX_SENSORS){ server.send(400,"text/plain","Bad index"); return; }
  sensors[i].enabled = false; sensors[i].ip = ""; sensors[i].name = ""; sensors[i].isMain = false;
  saveSensors();
  // clear RAM history
  for(int s=0;s<HISTORY_SLOTS;s++){ histBuf[i][s].ts=0; histBuf[i][s].temp=NAN; histBuf[i][s].hum=NAN; histBuf[i][s].setTemp=0; }
  histIdx[i]=0;
  server.send(200,"text/plain","OK");
}

void handleAdj(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  int i = server.arg("i").toInt();
  String type = server.arg("type");
  float v = server.arg("v").toFloat();
  if(i<0 || i>=MAX_SENSORS || !sensors[i].enabled){ server.send(400,"text/plain","Bad"); return; }
  if(type=="set") sensors[i].setTemp += v;
  else if(type=="hys") sensors[i].hysteresis += v;
  else if(type=="offt") sensors[i].tempOffset += v;
  else if(type=="offh") sensors[i].humOffset += v;
  if(sensors[i].hysteresis < 0) sensors[i].hysteresis = 0;
  saveSensors();
  server.send(200,"text/plain","OK");
}

void handleSetMain(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  int idx = server.arg("i").toInt();
  if(idx<0 || idx>=MAX_SENSORS || !sensors[idx].enabled){ server.send(400,"text/plain","Bad"); return; }
  for(int i=0;i<MAX_SENSORS;i++) sensors[i].isMain = false;
  sensors[idx].isMain = true;
  saveSensors();
  server.send(200,"text/plain","OK");
}

void handleSetName(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  int idx = server.arg("i").toInt();
  String name = server.arg("name");
  if(idx<0 || idx>=MAX_SENSORS || !sensors[idx].enabled){ server.send(400,"text/plain","Bad"); return; }
  sensors[idx].name = name;
  saveSensors();
  server.send(200,"text/plain","OK");
}

void handleHistoryJson_prot(){ if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication(); handleHistoryJson(); }

void handleSaveWifi(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  savedSSID = server.arg("s");
  savedPASS = server.arg("p");
  apPass = server.arg("ap");
  staticIP = server.arg("ip");
  gatewayIP = server.arg("gw");
  subnetMask = server.arg("sm");
  saveWifiAuth();
  server.send(200,"text/plain","OK");
  delay(500);
  ESP.restart();
}
void handleSaveAuth(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  AUTH_USER = server.arg("u"); AUTH_PASS = server.arg("p");
  saveWifiAuth();
  server.send(200,"text/plain","OK");
  delay(500);
  ESP.restart();
}

void handleSavePrograms(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  for(int p=0;p<4;p++){
    programs[p].startHour = server.arg("p"+String(p)+"_start").toInt();
    programs[p].stopHour  = server.arg("p"+String(p)+"_stop").toInt();
    programs[p].setTemp   = server.arg("p"+String(p)+"_temp").toFloat();
    programs[p].enabled   = (server.arg("p"+String(p)+"_en") == "1");
  }
  savePrograms();
  server.send(200,"text/plain","OK");
}

void handleSaveDebounce(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  int d = server.arg("deb").toInt();
  if(d<0) d=0;
  debounceSeconds = (unsigned long)d * 1000UL;
  saveWifiAuth();
  server.send(200,"text/plain","OK");
}

// set timezone offset (minutes)
void handleSetTZ(){
  if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication();
  if(server.method() != HTTP_POST){ server.send(400,"text/plain","POST required"); return; }
  String s = server.arg("tz");
  long m = s.toInt();
  tzOffsetMinutes = m;
  saveWifiAuth();
  server.send(200,"text/plain","OK");
}

// ========== Setup & Loop ==========
void setup(){
  Serial.begin(115200);
  pinMode(RELAY_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  relayOff();

  loadWifiAuth();
  loadSensors();
  loadPrograms();
  initHistoryRam();

  // NTP
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_Relay", apPass.c_str());
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  if(savedSSID.length()>0){
    if(staticIP.length()>0 && gatewayIP.length()>0 && subnetMask.length()>0){
      IPAddress sip, gw, sm;
      sip.fromString(staticIP); gw.fromString(gatewayIP); sm.fromString(subnetMask);
      WiFi.config(sip, gw, sm);
    }
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    unsigned long t0 = millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t0 < 10000) delay(200);
    if(WiFi.status()==WL_CONNECTED){
      Serial.print("STA IP: "); Serial.println(WiFi.localIP());
      delay(3000);
      WiFi.softAPdisconnect(true);
    }
  }

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/sensors_json", HTTP_GET, handleSensorsJson_prot);
  server.on("/history.json", HTTP_GET, handleHistoryJson_prot);

  server.on("/on", HTTP_GET, handleOn);
  server.on("/off", HTTP_GET, handleOff);
  server.on("/state", HTTP_GET, handleState);

  server.on("/addSensor", HTTP_GET, handleAddSensor);
  server.on("/removeSensor", HTTP_GET, handleRemoveSensor);
  server.on("/adj", HTTP_GET, handleAdj);
  server.on("/setMain", HTTP_GET, handleSetMain);
  server.on("/setName", HTTP_GET, handleSetName);

  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/saveauth", HTTP_POST, handleSaveAuth);
  server.on("/savePrograms", HTTP_POST, handleSavePrograms);
  server.on("/saveDebounce", HTTP_POST, handleSaveDebounce);
  server.on("/settz", HTTP_POST, handleSetTZ);
  server.on("/time.json", HTTP_GET, handleTimeJson);

  server.on("/reset", HTTP_GET, [](void){ if(!server.authenticate(AUTH_USER.c_str(), AUTH_PASS.c_str())) return server.requestAuthentication(); factoryReset(); });

  server.begin();
  Serial.println("Server started");
}

uint32_t lastStorageMinute = 0;
unsigned long lastPoll = 0;

void applySchedulesIfAny(){
  time_t nowt = time(NULL);
  struct tm tmnow;
  localtime_r(&nowt, &tmnow);
  int curHour = tmnow.tm_hour;
  for(int p=0;p<4;p++){
    if(!programs[p].enabled) continue;
    int sh = programs[p].startHour;
    int eh = programs[p].stopHour;
    bool inRange = false;
    if(sh <= eh) inRange = (curHour >= sh && curHour < eh);
    else inRange = (curHour >= sh || curHour < eh);
    if(inRange){
      for(int i=0;i<MAX_SENSORS;i++){
        if(sensors[i].enabled && sensors[i].isMain){
          sensors[i].setTemp = programs[p].setTemp;
        }
      }
      break;
    }
  }
  saveSensors();
}

void maybeStoreHistory(){
  time_t nowt = time(NULL);
  if(nowt <= 0) return;
  uint32_t minuteIndex = nowt / 60;
  if(minuteIndex == lastStorageMinute) return;
  if((minuteIndex % HISTORY_INTERVAL_MIN) == 0){
    for(int i=0;i<MAX_SENSORS;i++){
      if(!sensors[i].enabled) continue;
      uint32_t ts = (uint32_t)nowt + tzOffsetMinutes*60;
      float t = sensors[i].lastTemp;
      float h = sensors[i].lastHum;
      float s = sensors[i].setTemp;
      appendHistoryRam(i, ts, t, h, s);
    }
  }
  lastStorageMinute = minuteIndex;
}

void evaluateDesiredRelay(){
  if(desiredRelayState != relayState){
    if(millis() - lastDesiredRelayChange >= debounceSeconds){
      if(desiredRelayState) relayOn(); else relayOff();
      Serial.printf("Debounce applied new relay state: %d\n", desiredRelayState);
    }
  }
}

void loop(){
  server.handleClient();

  // Button handling: short toggle, long reset
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if(pressed && !buttonPressed){ buttonPressed=true; pressStartTime = millis(); }
  else if(!pressed && buttonPressed){
    unsigned long duration = millis() - pressStartTime;
    buttonPressed=false;
    if(duration < 2000){
      desiredRelayState = !relayState;
      lastDesiredRelayChange = millis();
    } else if(duration >= 10000){
      factoryReset();
    }
  }

  unsigned long now = millis();
  // Poll sensors periodically
  if(now - lastPoll >= POLL_INTERVAL_MS){
    lastPoll = now;
    uint32_t ts = time(NULL) + tzOffsetMinutes*60;
    for(int i=0;i<MAX_SENSORS;i++){
      if(sensors[i].enabled){
        if(pollSensor(i)){
          appendHistoryRam(i, ts, sensors[i].lastTemp, sensors[i].lastHum, sensors[i].setTemp);
        }
      }
    }
    applySchedulesIfAny();
  }

  // Relay control by main sensor hysteresis (if main exists)
  int mainIdx = -1;
  for(int i=0;i<MAX_SENSORS;i++) if(sensors[i].enabled && sensors[i].isMain){ mainIdx = i; break; }
  if(mainIdx >= 0 && !isnan(sensors[mainIdx].lastTemp)){
    float t = sensors[mainIdx].lastTemp;
    float setv = sensors[mainIdx].setTemp;
    float hys = sensors[mainIdx].hysteresis;
    if(t >= setv + hys) { desiredRelayState = false; lastDesiredRelayChange = lastDesiredRelayChange==0 ? millis() : lastDesiredRelayChange; }
    else if(t <= setv - hys) { desiredRelayState = true; lastDesiredRelayChange = lastDesiredRelayChange==0 ? millis() : lastDesiredRelayChange; }
  }

  evaluateDesiredRelay();
  maybeStoreHistory();

  delay(10);
}
