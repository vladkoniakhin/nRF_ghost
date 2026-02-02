#include "WebPortalManager.h"
#include "System.h"
#include "ConfigManager.h" 

struct WebSpiLock {
    WebSpiLock() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)); }
    ~WebSpiLock() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

// Basic check to prevent going out of root
bool validatePath(String& path) {
    if (path.indexOf("..") != -1) return false;
    if (path.indexOf("//") != -1) return false;
    if (path.length() > 64) return false;
    if (!path.startsWith("/")) path = "/" + path;
    return true;
}

// WebSocket Event Handler (Minimal)
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {}

// --- FULL HTML ---
static const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
 <title>nRF Ghost v5.1</title>
 <meta name="viewport" content="width=device-width, initial-scale=1">
 <style>
  body { font-family: monospace; background: #0d0d0d; color: #00ff00; margin: 0; padding: 15px; }
  h2 { border-bottom: 2px solid #004400; padding-bottom: 10px; color: #00ff00; text-transform: uppercase; }
  .card { background: #1a1a1a; padding: 15px; margin-bottom: 15px; border: 1px solid #333; border-radius: 4px; }
  button { background: #005500; color: white; border: 1px solid #007700; padding: 10px 20px; cursor: pointer; margin: 5px 5px 0 0; text-transform: uppercase; font-weight: bold; }
  button:hover { background: #007700; }
  button:active { background: #009900; }
  input, textarea { background: #000; color: #0f0; border: 1px solid #444; padding: 8px; width: 100%; box-sizing: border-box; font-family: monospace; }
  textarea { height: 250px; resize: vertical; }
  canvas { width: 100%; height: 120px; background: #000; border: 1px solid #444; display: block; }
  #status { float: right; font-size: 0.8em; }
  .warn { color: #ffaa00; }
  .err { color: #ff3333; }
  table { width: 100%; border-collapse: collapse; font-size: 0.9em; }
  td { padding: 6px; border-bottom: 1px solid #333; }
  a { color: #0af; text-decoration: none; }
 </style>
</head>
<body>
 <h2>Ghost v5.1 <span id="status" class="warn">[CONNECTING]</span></h2>

 <div class="card">
  <h3>Live Spectrum & Status</h3>
  <canvas id="chart"></canvas>
  <div style="margin-top:5px; font-size: 0.9em;">
    LOG: <span id="log" style="color:#aaa">...</span> | MEM: <span id="mem">-</span>
  </div>
 </div>

 <div class="card">
  <h3>GhostScript Editor</h3>
  <textarea id="editor" placeholder="// Example Script
WAIT_RX 433.92
IF_SIGNAL 0xA1B2C3
LOG 'Target Found'
TX_RAW /jam.sub"></textarea>
  <br>
  <button onclick="saveFile()">Save</button>
  <button onclick="runScript()">Run Script</button>
  <button onclick="stop()" style="background:#550000; border-color:#770000;">STOP ALL</button>
 </div>

 <div class="card">
  <h3>Storage (SD)</h3>
  <button onclick="loadFiles()">Refresh</button>
  <div id="filelist" style="margin-top:10px;"></div>
 </div>

 <script>
  var ws = new WebSocket('ws://' + window.location.hostname + '/ws');
  var ctx = document.getElementById('chart').getContext('2d');
  
  ws.onopen = function() { 
    document.getElementById('status').innerText = '[ONLINE]'; 
    document.getElementById('status').className = '';
    document.getElementById('status').style.color = '#00ff00';
  };
  ws.onclose = function() { 
    document.getElementById('status').innerText = '[OFFLINE]'; 
    document.getElementById('status').className = 'err';
  };
  
  ws.onmessage = function(evt) {
    if (typeof evt.data === "string") {
        try {
            var d = JSON.parse(evt.data);
            if(d.t === 's') { 
                document.getElementById('log').innerText = d.l;
                document.getElementById('mem').innerText = d.m;
            }
        } catch(e){}
    } else {
        var arr = new Uint8Array(evt.data);
        drawSpectrum(arr);
    }
  };

  function drawSpectrum(data) {
    var w = ctx.canvas.width;
    var h = ctx.canvas.height;
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, w, h); 
    ctx.beginPath();
    ctx.strokeStyle = '#00ff00';
    ctx.lineWidth = 1;
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
   fd.append("data", new Blob([txt], { type: 'text/plain' }), "script.txt");
   fetch('/api/fs/write', { method: 'POST', body: fd }).then(r=>{
       if(r.ok) alert('Saved'); else alert('Save Failed');
   });
  }
  
  function runScript() { fetch('/api/run_script?path=/script.txt'); }
  function stop() { fetch('/api/stop'); }

  function loadFiles() {
   fetch('/list').then(r => r.json()).then(j => {
    let h = '<table>';
    j.forEach(f => {
     h += `<tr><td>${f.n}</td><td>${f.s}</td><td><a href="/dl?n=${f.n}">DL</a> <a href="#" onclick="del('${f.n}')" class="err">RM</a></td></tr>`;
    });
    document.getElementById('filelist').innerHTML = h + '</table>';
   });
  }
  
  function del(n) { if(confirm('Delete ' + n + '?')) fetch('/del?n='+n, {method:'DELETE'}).then(loadFiles); }

  loadFiles();
  fetch('/api/fs/read?path=/script.txt').then(r=>r.text()).then(t=>document.getElementById('editor').value=t);
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
    
    // Config Integration
    String ssid = ConfigManager::getInstance().getWifiSsid();
    String pass = ConfigManager::getInstance().getWifiPass();
    
    WiFi.mode(WIFI_AP);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    
    if (pass.length() > 0) WiFi.softAP(ssid.c_str(), pass.c_str());
    else WiFi.softAP(ssid.c_str());
    
    _dnsServer.start(53, "*", apIP);

    // WebSocket
    _ws.onEvent(onEvent);
    _server.addHandler(&_ws);

    // --- ENDPOINTS ---

    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });

    _server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest *r){
        r->send(200);
    });

    _server.on("/api/run_script", HTTP_GET, [](AsyncWebServerRequest *r){
        if(r->hasParam("path")) { 
            r->send(200); 
        } else r->send(400);
    });

    // LIST FILES (Fully Unlocked)
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

    // DOWNLOAD FILE (Fully Unlocked)
    _server.on("/dl", HTTP_GET, [](AsyncWebServerRequest *r){
        if(r->hasParam("n")){
            String path = "/" + r->getParam("n")->value();
            if (!validatePath(path)) { r->send(400); return; }
            
            WebSpiLock lock;
            if (lock.locked()) {
                r->send(SD, path, "application/octet-stream");
            } else r->send(503);
        }
    });

    // DELETE FILE (Fully Unlocked)
    _server.on("/del", HTTP_DELETE, [](AsyncWebServerRequest *r){
         if(r->hasParam("n")){
            String path = "/" + r->getParam("n")->value();
            if (!validatePath(path)) { r->send(400); return; }
            
            WebSpiLock lock;
            if (lock.locked()) {
                SD.remove(path);
                r->send(200);
            } else r->send(503);
         }
    });

    // UPLOAD FILE
    _server.on("/api/fs/write", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
        String path = "/script.txt"; 
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

void WebPortalManager::broadcastStatus(const char* state, int mem) {
    if (!_isRunning) return;
    char json[128];
    snprintf(json, sizeof(json), "{\"t\":\"s\",\"l\":\"%s\",\"m\":%d}", state, mem);
    _ws.textAll(json);
}

void WebPortalManager::broadcastSpectrum(uint8_t* data, size_t len) {
    if (!_isRunning) return;
    _ws.binaryAll(data, len);
}

void WebPortalManager::processDns() { if (_isRunning) _dnsServer.processNextRequest(); _ws.cleanupClients(); }
bool WebPortalManager::isRunning() const { return _isRunning; }
void WebPortalManager::saveCreds(const char* ssid, const char* pass) {}