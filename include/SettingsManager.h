#pragma once
#include "Common.h"
#include <Preferences.h>

class SettingsManager {
public:
    static SettingsManager& getInstance();
   
    void init();
    void load();
    void save();

    // Геттеры/Сеттеры
    bool getLedEnabled() const { return _settings.ledEnabled; }
    void setLedEnabled(bool v) { _settings.ledEnabled = v; save(); }

    uint8_t getBrightness() const { return _settings.brightness; }
    void setBrightness(uint8_t v) { _settings.brightness = v; save(); }

    uint8_t getDefaultChannel() const { return _settings.defaultChannel; }

private:
    SettingsManager();
    Preferences _prefs;
    DeviceSettings _settings;
};