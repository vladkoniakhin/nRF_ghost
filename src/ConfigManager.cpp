#include "ConfigManager.h"
#include "System.h" 

struct CfgSpiLock {
    CfgSpiLock() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)); }
    ~CfgSpiLock() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {}

void ConfigManager::init() {
    if (!loadFromFile()) {
        Serial.println("[CFG] Failed to load. Using defaults.");
        loadDefaults();
        save(); 
    } else {
        Serial.println("[CFG] Loaded OK.");
    }
}

void ConfigManager::loadDefaults() {
    _wifiSsid = "nRF_Admin";
    _wifiPass = "ghost1234";
    _ledBrightness = 50;
    _defaultAttack = 0;
}

bool ConfigManager::loadFromFile() {
    CfgSpiLock lock;
    if (!lock.locked()) return false;

    if (!SD.exists(_filename)) return false;

    File file = SD.open(_filename, FILE_READ);
    if (!file) return false;
    
    if (file.size() == 0) {
        file.close();
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("[CFG] JSON Error: %s\n", error.c_str());
        return false;
    }

    if (doc["wifi_ssid"].is<const char*>()) _wifiSsid = doc["wifi_ssid"].as<String>();
    if (doc["wifi_pass"].is<const char*>()) _wifiPass = doc["wifi_pass"].as<String>();
    if (doc["led_brightness"].is<uint8_t>()) _ledBrightness = doc["led_brightness"].as<uint8_t>();
    if (doc["default_attack_mode"].is<int>()) _defaultAttack = doc["default_attack_mode"].as<int>();

    return true;
}

void ConfigManager::save() {
    CfgSpiLock lock;
    if (!lock.locked()) return;

    JsonDocument doc;
    doc["wifi_ssid"] = _wifiSsid;
    doc["wifi_pass"] = _wifiPass;
    doc["led_brightness"] = _ledBrightness;
    doc["default_attack_mode"] = _defaultAttack;

    // Safe save strategy: write to tmp, then rename could be better, 
    // but SD library support varies. Using direct overwrite for MVP stability.
    if (SD.exists(_filename)) SD.remove(_filename);

    File file = SD.open(_filename, FILE_WRITE);
    if (!file) {
        Serial.println("[CFG] Write Error");
        return;
    }

    serializeJson(doc, file);
    file.close();
    Serial.println("[CFG] Saved.");
}

String ConfigManager::getWifiSsid() const { return _wifiSsid; }
String ConfigManager::getWifiPass() const { return _wifiPass; }
uint8_t ConfigManager::getLedBrightness() const { return _ledBrightness; }
int ConfigManager::getDefaultAttackMode() const { return _defaultAttack; }

void ConfigManager::setWifiSsid(const String& ssid) { _wifiSsid = ssid; }
void ConfigManager::setWifiPass(const String& pass) { _wifiPass = pass; }