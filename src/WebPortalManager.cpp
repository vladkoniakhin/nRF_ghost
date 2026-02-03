#include "WebPortalManager.h"
#include "System.h"
#include "ConfigManager.h" 

// Локальный лок для веба
struct WebSpiLock {
    WebSpiLock() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)); }
    ~WebSpiLock() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

// Защита путей (Path Traversal Protection)
bool validatePath(String& path) {
    if (path.indexOf("..") != -1) return false;
    if (path.indexOf("//") != -1) return false;
    if (path.length() > 64) return false;
    if (!path.startsWith("/")) path = "/" + path;
    return true;
}

static const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>nRF Admin</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>body{font-family:Arial;text-align:center;background:#222;color:#fff}
.btn{background:#d00;color:white;padding:15px;margin:10px;border:none;border-radius:5px;width:80%}
</style></head><body><h1>nRF Ghost Admin</h1>
<div id="log">Status: Connecting...</div>
<script>
var ws = new WebSocket('ws://' + location.hostname + '/ws');
ws.onmessage = function(event) { document.getElementById('log').innerText = event.data; };
</script></body></html>
)rawliteral";

WebPortalManager& WebPortalManager::getInstance() {
    static WebPortalManager instance;
    return instance;
}

WebPortalManager::WebPortalManager() : _server(80), _ws("/ws"), _isRunning(false) {}

void WebPortalManager::start(const char* ssid) {
    if (_isRunning) stop();

    // WiFi AP уже поднят SystemController, тут только конфигурируем IP
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    
    if (ssid == nullptr || strlen(ssid) == 0) {
        WiFi.softAP("nRF_Admin", "ghost1234");
    } else {
        WiFi.softAP(ssid); // Evil Twin Mode
    }

    _dnsServer.start(53, "*", IPAddress(192,168,4,1));

    // WebSocket Init with client limit
    _ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len){
        if (type == WS_EVT_CONNECT) {
            // FIX v7.0: Limit clients to prevent Heap exhaustion
            if (server->count() > 4) {
                client->close();
            }
        }
    });
    _server.addHandler(&_ws);

    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    _server.on("/api/fs/read", HTTP_GET, [](AsyncWebServerRequest *r){
        if(!r->hasParam("path")) { r->send(400); return; }
        String path = r->getParam("path")->value();
        if(!validatePath(path)) { r->send(403); return; }
        
        // В v7.0 мы полагаемся на то, что NRF выключен, поэтому SPI относительно свободен.
        if(SD.exists(path)) {
            r->send(SD, path, "application/octet-stream");
        } else {
            r->send(404);
        }
    });

    _server.begin(); 
    _isRunning = true;
}

void WebPortalManager::stop() {
    if (_isRunning) { 
        _dnsServer.stop(); 
        _server.end(); 
        _ws.cleanupClients();
        // WiFi не выключаем, это дело System
        _isRunning = false; 
    }
}

static char g_wsBuffer[128];

void WebPortalManager::broadcastStatus(const char* state, int mem) {
    if (!_isRunning) return;
    snprintf(g_wsBuffer, sizeof(g_wsBuffer), "{\"s\":\"%s\",\"m\":%d}", state, mem);
    _ws.textAll(g_wsBuffer);
}

void WebPortalManager::processDns() {
    if (_isRunning) {
        _dnsServer.processNextRequest();
        _ws.cleanupClients();
    }
}

bool WebPortalManager::isRunning() const { return _isRunning; }