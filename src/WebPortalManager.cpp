#include "WebPortalManager.h"

// --- v3.0 Web Assets (Embedded HTML/JS) ---
// В реальном проекте лучше вынести в файловую систему, но для надежности MVP держим в PROGMEM
static const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
 <title>nRF Ghost Pro Admin</title>
 <meta name="viewport" content="width=device-width, initial-scale=1">
 <style>
  body { font-family: monospace; background: #121212; color: #00ff00; margin: 0; padding: 20px; }
  h2 { border-bottom: 1px solid #333; padding-bottom: 10px; }
  .card { background: #1e1e1e; padding: 15px; margin-bottom: 20px; border-radius: 5px; border: 1px solid #333; }
  button { background: #006600; color: white; border: none; padding: 10px 20px; cursor: pointer; margin-right: 5px; }
  button:hover { background: #008800; }
  input, textarea { background: #333; color: white; border: 1px solid #555; padding: 5px; width: 100%; box-sizing: border-box; }
  textarea { height: 300px; font-family: monospace; }
  #status { font-weight: bold; color: #ff00ff; }
  table { width: 100%; border-collapse: collapse; }
  td { padding: 8px; border-bottom: 1px solid #333; }
  a { color: #0af; text-decoration: none; }
 </style>
</head>
<body>
 <h2>nRF Ghost v3.0 <span id="status">[CONNECTING...]</span></h2>

 <div class="card">
  <h3>System Status</h3>
  <div id="log" style="height: 50px; overflow: hidden; color: #aaa;">Waiting for logs...</div>
 </div>

 <div class="card">
  <h3>DuckyScript Editor (/badusb.txt)</h3>
  <textarea id="editor"></textarea>
  <br><br>
  <button onclick="saveFile()">💾 SAVE</button>
  <button onclick="loadFile()">🔄 RELOAD</button>
 </div>

 <div class="card">
  <h3>File Manager</h3>
  <div id="filelist">Loading...</div>
 </div>

 <script>
  // Live Status Polling
  setInterval(() => {
   fetch('/api/status').then(r => r.json()).then(d => {
    document.getElementById('status').innerText = d.state;
    document.getElementById('log').innerText = d.log;
   });
  }, 1000);

  // Editor Functions
  function loadFile() {
   fetch('/api/fs/read?path=/badusb.txt').then(r => r.text()).then(t => {
    document.getElementById('editor').value = t;
   });
  }

  function saveFile() {
   const txt = document.getElementById('editor').value;
   const fd = new FormData();
   fd.append("data", txt);
   fetch('/api/fs/write', { method: 'POST', body: fd }).then(() => alert('Saved!'));
  }

  // File Manager
  function loadFiles() {
   fetch('/list').then(r => r.json()).then(j => {
    let h = '<table>';
    j.forEach(f => {
     h += `<tr><td>${f.n}</td><td>${f.s} b</td><td><a href="/dl?n=${f.n}">DL</a> <a href="#" onclick="del('${f.n}')">DEL</a></td></tr>`;
    });
    document.getElementById('filelist').innerHTML = h + '</table>';
   });
  }

  // Init
  loadFile();
  loadFiles();
 </script>
</body>
</html>
)rawliteral";

WebPortalManager& WebPortalManager::getInstance() {
    static WebPortalManager instance;
    return instance;
}

WebPortalManager::WebPortalManager() : _server(80), _isRunning(false) {}

void WebPortalManager::start(const char* ssid) {
    if (_isRunning) stop();
    
    WiFi.mode(WIFI_AP);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    
    char buf[33]; 
    strncpy(buf, ssid, 32); 
    buf[32] = 0;
    WiFi.softAP(buf);
    
    _dnsServer.start(53, "*", apIP);

    // --- ENDPOINTS ---

    // 1. UI
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ 
        r->send_P(200, "text/html", index_html); 
    });

    // 2. STATUS API (Live Polling)
    _server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r){
        // В реальном коде данные брать из SystemController::getStatus()
        // Здесь эмуляция для примера
        String json = "{\"state\":\"ADMIN_MODE\",\"log\":\"System Ready\",\"mem\":" + String(ESP.getFreeHeap()) + "}";
        r->send(200, "application/json", json);
    });

    // 3. EDITOR API (Read)
    _server.on("/api/fs/read", HTTP_GET, [](AsyncWebServerRequest *r){
        if (r->hasParam("path")) {
            String path = r->getParam("path")->value();
            if (xSemaphoreTake(g_spiMutex, 2000)) {
                if (SD.exists(path)) r->send(SD, path, "text/plain");
                else r->send(404, "text/plain", "File not found");
                xSemaphoreGive(g_spiMutex);
            } else r->send(503, "text/plain", "SPI Busy");
        } else r->send(400);
    });

    // 4. EDITOR API (Write)
    // Используем raw upload handler для сохранения
    _server.on("/api/fs/write", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
        // Хардкод пути для MVP, в идеале брать из параметров
        String path = "/badusb.txt"; 
        
        if(!index) {
            if(xSemaphoreTake(g_spiMutex, 2000)) {
                // Если файл существует - удаляем перед перезаписью
                if(SD.exists(path)) SD.remove(path);
                r->_tempFile = SD.open(path, FILE_WRITE);
                xSemaphoreGive(g_spiMutex);
            }
        }
        if(r->_tempFile && xSemaphoreTake(g_spiMutex, 200)) {
            r->_tempFile.write(data, len);
            xSemaphoreGive(g_spiMutex);
        }
        if(final && r->_tempFile) {
            if(xSemaphoreTake(g_spiMutex, 200)) {
                r->_tempFile.close();
                xSemaphoreGive(g_spiMutex);
            }
        }
    });

    // 5. LEGACY FILE MANAGER (List/DL/Del)
    _server.on("/list", HTTP_GET, [](AsyncWebServerRequest *r){
        String json = "[";
        if (xSemaphoreTake(g_spiMutex, 2000)) {
            File root = SD.open("/");
            if(root) {
                File file = root.openNextFile();
                bool first = true;
                while(file){
                    if(!first) json += ",";
                    String name = String(file.name());
                    if (name.startsWith("/")) name = name.substring(1);
                    json += "{\"n\":\"" + name + "\",\"s\":\"" + String(file.size()) + "\"}";
                    first = false;
                    file = root.openNextFile();
                }
            }
            xSemaphoreGive(g_spiMutex);
        }
        json += "]";
        r->send(200, "application/json", json);
    });

    _server.on("/dl", HTTP_GET, [](AsyncWebServerRequest *r){
        if(r->hasParam("n")){
            String path = "/" + r->getParam("n")->value();
            if (xSemaphoreTake(g_spiMutex, 2000)) {
                r->send(SD, path, "application/octet-stream");
                xSemaphoreGive(g_spiMutex);
            } else r->send(503);
        }
    });

    _server.on("/del", HTTP_DELETE, [](AsyncWebServerRequest *r){
         if(r->hasParam("n")){
            String path = "/" + r->getParam("n")->value();
            if (xSemaphoreTake(g_spiMutex, 2000)) {
                SD.remove(path);
                xSemaphoreGive(g_spiMutex);
                r->send(200);
            }
         }
    });

    _server.begin(); 
    _isRunning = true;
}

void WebPortalManager::stop() {
    if (_isRunning) { 
        _dnsServer.stop(); 
        _server.end(); 
        WiFi.softAPdisconnect(true); 
        WiFi.mode(WIFI_STA); 
        _isRunning = false; 
    }
}

void WebPortalManager::processDns() { if (_isRunning) _dnsServer.processNextRequest(); }
bool WebPortalManager::isRunning() const { return _isRunning; }

void WebPortalManager::saveCreds(const char* ssid, const char* pass) {
    // Legacy placeholder
}