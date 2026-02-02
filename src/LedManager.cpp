#include "LedManager.h"
#include "SettingsManager.h" // FIX: Подключаем менеджер настроек

LedManager& LedManager::getInstance() { static LedManager i; return i; }

LedManager::LedManager() : 
    _pixels(Config::NEOPIXEL_COUNT, Config::PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800), 
    _currentState(SystemState::IDLE),
    _lastState(SystemState::IDLE), 
    _handshakeCaptured(false),
    _rollingCode(false),
    _lastUpdate(0),
    _animStep(0),
    _blinkState(false)
{}

void LedManager::init() { 
    _pixels.begin(); 
    _pixels.setBrightness(50); 
    _pixels.clear(); 
    _pixels.show(); 
}

void LedManager::setStatus(const StatusMessage& msg) {
    _currentState = msg.state;
    _handshakeCaptured = msg.handshakeCaptured;
    _rollingCode = msg.rollingCodeDetected;
    
    if (_currentState != _lastState) {
        _lastState = _currentState;
        _lastUpdate = 0; 
        _blinkState = false;
        _animStep = 0;
        _pixels.clear(); 
        _pixels.show(); 
    }
}

void LedManager::update() {
    // FIX v6.2: Stealth Mode Check
    // Если в настройках LED отключен — принудительно гасим и выходим.
    if (!SettingsManager::getInstance().getLedEnabled()) {
        _pixels.clear();
        _pixels.show();
        return;
    }

    if (_handshakeCaptured) { runRainbow(); return; }
    if (_rollingCode) { runBlink(255, 100, 0, 200); return; }

    switch (_currentState) {
        case SystemState::IDLE: setSolid(0, 0, 20); break;
        case SystemState::SYS_ERROR:
        case SystemState::SD_ERROR: setSolid(50, 0, 0); break;

        case SystemState::ADMIN_MODE: runBlink(0, 50, 50, 1000); break;
        case SystemState::WEB_CLIENT_CONNECTED: runBreathe(0, 255, 255); break;

        case SystemState::SCANNING: runBlink(0, 255, 0, 300); break;
        case SystemState::ATTACKING_WIFI_DEAUTH: runStrobe(255, 0, 0); break;
        case SystemState::ATTACKING_WIFI_SPAM: runBlink(255, 200, 0, 500); break;
        case SystemState::ATTACKING_EVIL_TWIN: setSolid(100, 0, 200); break;

        case SystemState::ANALYZING_SUBGHZ_RX: runBlink(0, 0, 255, 100); break;
        case SystemState::ATTACKING_SUBGHZ_TX: setSolid(255, 100, 0); break;

        case SystemState::SNIFFING_NRF: setSolid(20, 20, 20); break;
        case SystemState::ATTACKING_NRF: runStrobe(200, 0, 0); break; 
        case SystemState::ATTACKING_MOUSEJACK: runBlink(255, 0, 50, 100); break;

        case SystemState::ATTACKING_BLE: runBreathe(0, 0, 100); break;

        default: setSolid(5, 5, 5); break;
    }
}

void LedManager::setSolid(uint8_t r, uint8_t g, uint8_t b) {
    _pixels.setPixelColor(0, _pixels.Color(r, g, b)); _pixels.show();
}
void LedManager::runBlink(uint8_t r, uint8_t g, uint8_t b, int interval) {
    if (millis() - _lastUpdate > interval) {
        _lastUpdate = millis(); _blinkState = !_blinkState;
        if (_blinkState) _pixels.setPixelColor(0, _pixels.Color(r, g, b)); else _pixels.setPixelColor(0, 0);
        _pixels.show();
    }
}
void LedManager::runStrobe(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t cycle = millis() % 100;
    if (cycle < 20) _pixels.setPixelColor(0, _pixels.Color(r, g, b)); else _pixels.setPixelColor(0, 0);
    _pixels.show();
}
void LedManager::runBreathe(uint8_t r, uint8_t g, uint8_t b) {
    if (millis() - _lastUpdate > 20) { 
        _lastUpdate = millis(); float val = (exp(sin(millis()/2000.0*PI)) - 0.36787944)*108.0;
        _pixels.setPixelColor(0, _pixels.Color((r*(int)val)/255, (g*(int)val)/255, (b*(int)val)/255));
        _pixels.show();
    }
}
void LedManager::runRainbow() {
    if (millis() - _lastUpdate > 20) { _lastUpdate = millis(); _animStep++; _pixels.setPixelColor(0, Wheel(_animStep & 255)); _pixels.show(); }
}
uint32_t LedManager::Wheel(byte WheelPos) {
    WheelPos = 255 - WheelPos;
    if(WheelPos < 85) return _pixels.Color(255 - WheelPos * 3, 0, WheelPos * 3);
    if(WheelPos < 170) { WheelPos -= 85; return _pixels.Color(0, WheelPos * 3, 255 - WheelPos * 3); }
    WheelPos -= 170; return _pixels.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}