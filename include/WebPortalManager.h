#pragma once
#include "Common.h"
#include "Config.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>

class WebPortalManager {
public:
    static WebPortalManager& getInstance();
    void start(const char* ssid);
    void stop();
    void processDns();
    void broadcastStatus(const char* state, int mem);
    bool isRunning() const;

private:
    WebPortalManager();
    DNSServer _dnsServer;
    AsyncWebServer _server;
    AsyncWebSocket _ws; // FIX: Added WS member
    bool _isRunning;
};