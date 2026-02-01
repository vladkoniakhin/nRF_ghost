#include "SettingsManager.h"
SettingsManager& SettingsManager::getInstance() { static SettingsManager i; return i; }
SettingsManager::SettingsManager() {}
void SettingsManager::init() { _prefs.begin("nrfbox", false); load(); }
void SettingsManager::load() { _settings.ledEnabled=_prefs.getBool("led",true); _settings.brightness=_prefs.getUChar("bri",255); }
void SettingsManager::save() { _prefs.putBool("led",_settings.ledEnabled); _prefs.putUChar("bri",_settings.brightness); }