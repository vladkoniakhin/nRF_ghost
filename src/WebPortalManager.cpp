#include "WebPortalManager.h"
#include "System.h"
#include "ConfigManager.h" 

struct WebSpiLock {
    WebSpiLock() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)); }
    ~WebSpiLock() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

bool validatePath(String& path) {
    if (path.indexOf("..") != -1) return false;
    if (path.indexOf("//") != -1) return false;
    if (path.length() > 64) return false;
    if (!path.startsWith("/")) path = "/" + path;
    return true;
}

// --- WebSocket Event Handler ---
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] Client %u connected\n", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] Client %u disconnected\n", client->id());
    }
}

// --- v5.0 INTERFACE (WebSocket Enabled) ---
static const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
 <title>nRF Ghost v5.0</title>
 <meta name="viewport" content="width=device-width, initial-scale=1">
 <style>
  body { font-family: monospace; background: #121212; color: #00ff00; margin: 0; padding: 20px; }
  h2 { border-bottom: 1px solid #333; padding-bottom: 10px; color: #00ff00; }
  .card { background: #1e1e1e; padding: 15px; margin-bottom: 20px; border-radius: 5px; border: 1px solid #333; }
  button { background: #006600; color: white; border: none; padding: 10px 20px; cursor: pointer; margin-right: 5px; }
  canvas { width: 100%; height: 100px; background: #000; border: 1px solid #333; }
  #status { font-weight: bold; color: #ff00ff; }
  textarea { background: #333; color: white; width: 100%; height: 200px; border: 1px solid #555; }
  table { width: 100%; border-collapse: collapse; }
  td { padding: 8px; border-bottom: 1px solid #333; }
  a { color: #0af; text-decoration: none; }
 </style>
</head>
<body>
 <h2>nRF Ghost v5.0 <span id="status">[WS...]</span></h2>

 <div class="card">
  <h3>Spectrum / Live Status</h3>
  <canvas id="chart"></canvas>
  <div>Log: <span id="log">Waiting...</span> | Mem: <span id="mem">-</span></div>
 </div>

 <div class="card">
  <h3>GhostScript Editor</h3>
  <textarea id="editor" placeholder="WAIT_RX 433.92..."></textarea><br><br>
  <button onclick="saveFile()">💾 SAVE</button>
  <button onclick="runScript()">▶ RUN SCRIPT</button>
 </div>

 <div class="card">
  <h3>File Manager</h3>
  <div id="filelist">Loading...</div>
 </div>

 <script>
  var ws = new WebSocket('ws://' + window.location.hostname + '/ws');
  var ctx = document.getElementById('chart').getContext('2d');
  
  ws.onopen = function() { document.getElementById('status').innerText = '[ONLINE]'; };
  ws.onclose = function() { document.getElementById('status').innerText = '[OFFLINE]'; };
  
  ws.onmessage = function(evt) {
    if (typeof evt.data === "string") {
        var d = JSON.parse(evt.data);
        if(d.type === 'status') {
            document.getElementById('log').innerText = d.log;
            document.getElementById('mem').innerText = d.mem;
        }
    } else {
        // Binary Data (Spectrum)
        var arr = new Uint8Array(evt.data);
        drawSpectrum(arr);
    }
  };

  function drawSpectrum(data) {
    var w = ctx.canvas.width;
    var h = ctx.canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.beginPath();
    ctx.strokeStyle = '#00ff00';
    for(var i=0; i<data.length; i++) {
        var x = (i / data.length) * w;
        var y = h - (data[i] / 255 * h);
        if(i==0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }

  function saveFile() {
   var txt = document.getElementById('editor').value;
   var fd = new FormData();
   fd.append("data", new Blob([txt]), "script.txt");
   fetch('/api/fs/write', { method: 'POST', body: fd }).then(()=>alert('Saved'));
  }
  
  function runScript() {
    fetch('/api/run_script?path=/script.txt');
  }

  function loadFiles() {
   fetch('/list').then(r => r.json()).then(j => {
    let h = '<table>';
    j.forEach(f => {
     h += `<tr><td>${f.n}</td><td>${f.s}b</td><td><a href="/dl?n=${f.n}">DL</a></td></tr>`;
    });
    document.getElementById('filelist').innerHTML = h + '</table>';
   });
  }
  loadFiles();
 </script>
</body>
</html>
)rawliteral";

WebPortalManager& WebPortalManager::getInstance() { 
    static WebPortalManager instance; 
    return instance; 
}

WebPortalManager::WebPortalManager() : _server(80), _ws("/ws"), _isRunning(false) {}

void WebPortalManager::start(const char* ignore) {
    if (_isRunning) stop();
    
    String ssid = ConfigManager::getInstance().getWifiSsid();
    String pass = ConfigManager::getInstance().getWifiPass();
    
    WiFi.mode(WIFI_AP);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    if (pass.length() > 0) WiFi.softAP(ssid.c_str(), pass.c_str());
    else WiFi.softAP(ssid.c_str());
    
    _dnsServer.start(53, "*", apIP);

    // --- WebSocket Init ---
    _ws.onEvent(onEvent);
    _server.addHandler(&_ws);

    // --- HTTP Endpoints ---
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });

    _server.on("/api/run_script", HTTP_GET, [](AsyncWebServerRequest *r){
        if(r->hasParam("path")) {
            // Запуск скрипта через ScriptManager
            // ScriptManager::getInstance().runScript(...); // Нужно добавить include
            r->send(200, "text/plain", "Started");
        } else r->send(400);
    });

    // File Operations (Standard)
    _server.on("/list", HTTP_GET, [](AsyncWebServerRequest *r){
        String json = "[";
        WebSpiLock lock;
        if (lock.locked()) {
            File root = SD.open("/");
            if(root) {
                File file = root.openNextFile();
                bool first = true;
                while(file){
                    String name = String(file.name());
                    if (!name.startsWith("/.")) {
                        if(!first) json += ",";
                        if (name.startsWith("/")) name = name.substring(1);
                        json += "{\"n\":\"" + name + "\",\"s\":\"" + String(file.size()) + "\"}";
                        first = false;
                    }
                    file = root.openNextFile();
                }
            }
        }
        json += "]";
        r->send(200, "application/json", json);
    });

    _server.on("/api/fs/write", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
        String path = "/script.txt"; // Hardcoded for v5.0 PoC
        if(!index) {
            WebSpiLock lock; if(lock.locked()) { if(SD.exists(path)) SD.remove(path); r->_tempFile = SD.open(path, FILE_WRITE); }
        }
        if(r->_tempFile) { WebSpiLock lock; if(lock.locked()) r->_tempFile.write(data, len); }
        if(final && r->_tempFile) { WebSpiLock lock; if(lock.locked()) r->_tempFile.close(); }
    });

    _server.begin(); 
    _isRunning = true;
}

void WebPortalManager::stop() {
    if (_isRunning) { _dnsServer.stop(); _server.end(); WiFi.softAPdisconnect(true); _isRunning = false; }
}

// Push status to WebSocket clients (Call this from System loop)
void WebPortalManager::broadcastStatus(const char* state, int mem) {
    if (!_isRunning) return;
    char json[128];
    snprintf(json, sizeof(json), "{\"type\":\"status\",\"log\":\"%s\",\"mem\":%d}", state, mem);
    _ws.textAll(json);
}

void WebPortalManager::broadcastSpectrum(uint8_t* data, size_t len) {
    if (!_isRunning) return;
    _ws.binaryAll(data, len);
}

void WebPortalManager::processDns() { if (_isRunning) _dnsServer.processNextRequest(); _ws.cleanupClients(); }
bool WebPortalManager::isRunning() const { return _isRunning; }
void WebPortalManager::saveCreds(const char* ssid, const char* pass) {}