#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>

WebServer server(80);
Preferences prefs;
Adafruit_AHTX0 aht;

#define BUTTON_PIN 5

float tempOffset = 0.0;
float humOffset = 0.0;
float curTemp = 0.0;
float curHum = 0.0;

String defaultAPPass = "12345678";
String apPass;
String savedSSID = "";
String savedPASS = "";
String staticIP = "";

unsigned long pressStartTime = 0;
bool buttonPressed = false;

// ======== Helpers ========
String f1(float v){ return String(v,1); }

// ======== Button ========
void handleButton(){
  bool pressed = (digitalRead(BUTTON_PIN)==LOW);
  if(pressed && !buttonPressed){ buttonPressed=true; pressStartTime=millis();}
  else if(!pressed && buttonPressed){
    unsigned long duration = millis()-pressStartTime;
    buttonPressed=false;
    if(duration >= 10000){ // long press -> factory reset
      prefs.begin("sensor",false); prefs.clear(); prefs.end();
      prefs.begin("wifi",false); prefs.clear(); prefs.end();
      delay(500);
      ESP.restart();
    }
  }
}

// ======== Preferences ========
void loadPrefs(){
  prefs.begin("sensor",false);
  tempOffset = prefs.getFloat("toff",0.0);
  humOffset  = prefs.getFloat("hoff",0.0);
  prefs.end();

  prefs.begin("wifi",false);
  savedSSID = prefs.getString("ssid","");
  savedPASS = prefs.getString("pass","");
  apPass = prefs.getString("appass",defaultAPPass);
  staticIP = prefs.getString("ip","");
  prefs.end();
}

void savePrefs(){
  prefs.begin("sensor",false);
  prefs.putFloat("toff",tempOffset);
  prefs.putFloat("hoff",humOffset);
  prefs.end();
}

void saveWifi(String ssid,String pass,String ap,String ip){
  prefs.begin("wifi",false);
  prefs.putString("ssid",ssid);
  prefs.putString("pass",pass);
  prefs.putString("appass",ap);
  prefs.putString("ip",ip);
  prefs.end();
}

// ======== JSON ========
void handleJSON(){
  String js="{\"temp\":"+f1(curTemp)+",\"hum\":"+f1(curHum)+",\"toff\":"+f1(tempOffset)+",\"hoff\":"+f1(humOffset)+
            ",\"mac\":\""+WiFi.macAddress()+"\",\"ip\":\""+((WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():"N/A")+
            "\",\"rssi\":"+String(WiFi.RSSI())+"}";
  server.send(200,"application/json",js);
}

// ======== Web UI ========
void handleRoot(){
  String macStr = WiFi.macAddress();
  String ipStr  = (WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():"N/A";
  long rssi = WiFi.RSSI();
  String rssiStr = (WiFi.status()==WL_CONNECTED)?String(rssi)+" dBm":"N/A";

  String page = R"rawliteral(
<!doctype html><html>
<head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Sensor Node</title>
<style>
:root{--bg:#121212;--card:#1e1e1e;--accent:#6200EE;--muted:#bbbbbb;--white:#fff}
body{background:var(--bg);color:var(--white);margin:0;font-family:Roboto,Arial}
.header{padding:14px 16px;background:#1f1f1f;display:flex;gap:10px}
.tab{padding:8px 12px;color:var(--muted);cursor:pointer}
.tab.active{color:var(--white);background:#333;border-radius:8px}
.container{padding:16px}
.card{background:var(--card);padding:16px;border-radius:10px;margin-bottom:14px}
.btn{background:var(--accent);padding:10px;border:none;color:#fff;border-radius:8px;cursor:pointer;margin:4px}
.input{width:220px;padding:6px;margin:4px 0;border-radius:6px;border:1px solid #333;background:#101010;color:#fff;text-align:center}
.offset-btn{width:32px;height:32px;background:#6200EE;color:#fff;border:none;border-radius:8px;font-size:18px;margin:2px;cursor:pointer}
</style>
</head>
<body>
<div class='header'>
  <div class='tab active' id='tabSensor' onclick="showTab('sensor')">Sensor</div>
  <div class='tab' id='tabWifi' onclick="showTab('wifi')">Wi-Fi</div>
</div>
<div class='container'>

<div id='sensorTab'>
  <div class='card'>
    <h3>Sensor Data</h3>
    <p>Temperature: <span id='curTemp'>--</span>&deg;C 
      <button class='offset-btn' onclick="changeOffset('temp',-0.1)">-</button>
      <button class='offset-btn' onclick="changeOffset('temp',0.1)">+</button>
    </p>
    <p>Humidity: <span id='curHum'>--</span>% 
      <button class='offset-btn' onclick="changeOffset('hum',-0.1)">-</button>
      <button class='offset-btn' onclick="changeOffset('hum',0.1)">+</button>
    </p>
    <p>Temp Offset: <span id='toff'>0.0</span></p>
    <p>Hum Offset: <span id='hoff'>0.0</span></p>
  </div>
</div>

<div id='wifiTab' style='display:none'>
  <div class='card'>
    <h3>Wi-Fi Setup</h3>
    <label>SSID</label><input id='ssid' class='input'><br>
    <label>Password</label><input id='pass' class='input'><br>
    <label>AP Password</label><input id='appass' class='input'><br>
    <label>Static IP (optional)</label><input id='ip' class='input'><br>
    <button class='btn' onclick="saveWifi()">Save & Reboot</button>
    <h4>Factory Reset</h4>
    <button class='btn' style='background:#c62828' onclick="factoryReset()">Factory Reset</button>
    <hr>
    <p><b>MAC:</b>)rawliteral"+macStr+R"rawliteral(</p>
    <p><b>IP:</b>)rawliteral"+ipStr+R"rawliteral(</p>
    <p><b>RSSI:</b>)rawliteral"+rssiStr+R"rawliteral(</p>
  </div>
</div>

</div>
<script>
async function fetchJSON(path){try{let r=await fetch(path);return await r.json();}catch(e){return null;}}
async function updateUI(){
  let data=await fetchJSON('/json');
  if(data){
    // Only update sensor live data
document.getElementById('curTemp').innerText=data.temp.toFixed(1);
document.getElementById('curHum').innerText=data.hum.toFixed(1);
document.getElementById('toff').innerText=data.toff.toFixed(1);
document.getElementById('hoff').innerText=data.hoff.toFixed(1);

// Update Wi-Fi fields only once when page loads
if (!window.wifiFieldsLoaded) {
    document.getElementById('ssid').value=data.ssid || '';
    document.getElementById('pass').value=data.pass || '';
    document.getElementById('appass').value=data.ap || '';
    document.getElementById('ip').value=data.ip || '';
    window.wifiFieldsLoaded = true;
}

  }
}

// Offset buttons
function changeOffset(type,val){
  let url='/saveoffsets?'+type+'='+val;
  fetch(url).then(()=>updateUI());
}

async function saveWifi(){
  let s=document.getElementById('ssid').value;
  let p=document.getElementById('pass').value;
  let ap=document.getElementById('appass').value;
  let ip=document.getElementById('ip').value;
  await fetch('/savewifi?s='+s+'&p='+p+'&ap='+ap+'&ip='+ip);
  alert('Saved, rebooting');
}

async function factoryReset(){if(confirm('Reset to factory?')) await fetch('/reset');}

function showTab(t){
  document.getElementById('sensorTab').style.display=(t=='sensor'?'block':'none');
  document.getElementById('wifiTab').style.display=(t=='wifi'?'block':'none');
  document.getElementById('tabSensor').classList.toggle('active', t=='sensor');
  document.getElementById('tabWifi').classList.toggle('active', t=='wifi');
}

updateUI(); setInterval(updateUI,2000);
</script>
</body></html>
)rawliteral";

  server.send(200,"text/html",page);
}

// ======== Web Handlers ========
void handleSaveOffsets(){
  if(server.hasArg("temp")) tempOffset += server.arg("temp").toFloat();
  if(server.hasArg("hum")) humOffset += server.arg("hum").toFloat();
  savePrefs();
  server.send(200,"text/plain","OK");
}

void handleSaveWifi(){
  String s = server.arg("s");
  String p = server.arg("p");
  String ap = server.arg("ap");
  String ip = server.arg("ip");
  saveWifi(s,p,ap,ip);
  server.send(200,"text/plain","OK");
  delay(500); ESP.restart();
}

void handleReset(){
  prefs.begin("sensor",false); prefs.clear(); prefs.end();
  prefs.begin("wifi",false); prefs.clear(); prefs.end();
  delay(500); ESP.restart();
}

// ======== Setup ========
void setup(){
  Serial.begin(115200);
  Wire.begin();
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if(!aht.begin()){ Serial.println("AHT10/AHT20 not found"); }

  loadPrefs();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_Sensor",apPass.c_str());
  if(savedSSID.length()>0){
    if(staticIP.length()>0){
      IPAddress localIP;
      localIP.fromString(staticIP);
      WiFi.config(localIP, localIP, IPAddress(255,255,255,0));
    }
    WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
    unsigned long t0=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t0<10000) delay(200);
    if(WiFi.status()==WL_CONNECTED){
      Serial.print("STA IP: "); Serial.println(WiFi.localIP());
      delay(10000); WiFi.softAPdisconnect(true);
    }
  }

  server.on("/", handleRoot);
  server.on("/json", handleJSON);
  server.on("/saveoffsets", HTTP_GET, handleSaveOffsets);
  server.on("/savewifi", HTTP_GET, handleSaveWifi);
  server.on("/reset", handleReset);

  server.begin();
}

// ======== Loop ========
void loop(){
  server.handleClient();
  handleButton();

  sensors_event_t humidity, temp;
  if(aht.getEvent(&humidity,&temp)){
    curTemp = temp.temperature + tempOffset;
    curHum = humidity.relative_humidity + humOffset;
  }
}
