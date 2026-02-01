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
    bool isRunning() const;

private:
    WebPortalManager();
    DNSServer _dnsServer;
    AsyncWebServer _server;
    bool _isRunning;
    void saveCreds(const char* ssid, const char* pass);
};