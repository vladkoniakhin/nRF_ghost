#include "LedManager.h"
LedManager& LedManager::getInstance() { static LedManager i; return i; }
LedManager::LedManager() : _pixels(Config::NEOPIXEL_COUNT, Config::PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800), _lastState(SystemState::SYS_ERROR) {}

void LedManager::init() { _pixels.begin(); _pixels.setBrightness(50); _pixels.clear(); _pixels.show(); }
void LedManager::updateWithStatus(const StatusMessage& msg) {
    uint32_t c = 0;
    if(msg.handshakeCaptured) c = _pixels.Color(255,0,255);
    else if(msg.rollingCodeDetected) c = _pixels.Color(255,100,0);
    else switch(msg.state) {
        case SystemState::IDLE: c = _pixels.Color(0,0,50); break;
        case SystemState::SCANNING: c = _pixels.Color(0,50,0); break;
        case SystemState::ATTACKING_WIFI: case SystemState::ATTACKING_NRF: c = _pixels.Color(50,0,0); break;
        case SystemState::SYS_ERROR: c = _pixels.Color(50,0,0); break;
        default: c = _pixels.Color(10,10,10); break;
    }
    _pixels.setPixelColor(0, c); _pixels.show();
}