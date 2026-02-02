#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include "Common.h"

class ConfigManager {
public:
    static ConfigManager& getInstance();
    
    void init();
    
    String getWifiSsid() const;
    String getWifiPass() const;
    uint8_t getLedBrightness() const;
    int getDefaultAttackMode() const;

    void setWifiSsid(const String& ssid);
    void setWifiPass(const String& pass);
    void save();

private:
    ConfigManager();
    ConfigManager(const ConfigManager&) = delete;
    void operator=(const ConfigManager&) = delete;
    
    String _wifiSsid = "nRF_Admin";
    String _wifiPass = "ghost1234";
    uint8_t _ledBrightness = 50;
    int _defaultAttack = 0;

    const char* _filename = "/settings.json";
    
    void loadDefaults();
    bool loadFromFile();
};