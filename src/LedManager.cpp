#include "LedManager.h"

LedManager& LedManager::getInstance() { static LedManager i; return i; }

LedManager::LedManager() : 
    _pixels(Config::NEOPIXEL_COUNT, Config::PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800), 
    _currentState(SystemState::IDLE),
    _handshakeCaptured(false),
    _rollingCode(false),
    _lastUpdate(0),
    _animStep(0),
    _blinkState(false)
{}

void LedManager::init() { 
    _pixels.begin(); 
    _pixels.setBrightness(50); // Глобальная яркость 20% (чтобы не слепило)
    _pixels.clear(); 
    _pixels.show(); 
}

void LedManager::setStatus(const StatusMessage& msg) {
    _currentState = msg.state;
    _handshakeCaptured = msg.handshakeCaptured;
    _rollingCode = msg.rollingCodeDetected;
}

// Главный метод анимации (вызывать в loop)
void LedManager::update() {
    uint32_t now = millis();

    // 1. Приоритетные алерты
    if (_handshakeCaptured) {
        runRainbow(); // УСПЕХ!
        return;
    }
    
    if (_rollingCode) {
        runBlink(255, 100, 0, 200); // Оранжевое мигание (Warn)
        return;
    }

    // 2. Машина состояний
    switch (_currentState) {
        // --- IDLE & SYSTEM ---
        case SystemState::IDLE:
            setSolid(0, 0, 20); // Dim Blue
            break;
        case SystemState::SYS_ERROR:
        case SystemState::SD_ERROR:
            setSolid(50, 0, 0); // Brick Red Static
            break;

        // --- WEB ---
        case SystemState::ADMIN_MODE:
            runBlink(0, 50, 50, 1000); // Slow Cyan Blink (Waiting)
            break;
        case SystemState::WEB_CLIENT_CONNECTED:
            runBreathe(0, 255, 255); // Cyan Breathe (Active)
            break;

        // --- WIFI ---
        case SystemState::SCANNING:
            runBlink(0, 255, 0, 300); // Green Blink
            break;
        case SystemState::ATTACKING_WIFI_DEAUTH:
            runStrobe(255, 0, 0); // Red Strobe (Aggressive)
            break;
        case SystemState::ATTACKING_WIFI_SPAM:
            runBlink(255, 200, 0, 500); // Yellow Blink
            break;
        case SystemState::ATTACKING_EVIL_TWIN:
            setSolid(100, 0, 200); // Purple Static
            break;

        // --- SUB-GHZ ---
        case SystemState::ANALYZING_SUBGHZ_RX:
            runBlink(0, 0, 255, 100); // Fast Blue Blink (Data RX)
            break;
        case SystemState::ATTACKING_SUBGHZ_TX:
            setSolid(255, 100, 0); // Orange Static (TX Active)
            break;

        // --- NRF ---
        case SystemState::SNIFFING_NRF:
            setSolid(20, 20, 20); // Dim White (Stealth)
            break;
        case SystemState::ATTACKING_NRF: // Jamming
            runStrobe(200, 0, 0); 
            break;
        case SystemState::ATTACKING_MOUSEJACK:
            runBlink(255, 0, 50, 100); // Fast Red/Pink Flash
            break;

        // --- BLE ---
        case SystemState::ATTACKING_BLE:
            runBreathe(0, 0, 100); // Blue Breathe
            break;

        default:
            setSolid(5, 5, 5); // Off/Dim
            break;
    }
}

// --- ЭФФЕКТЫ (Non-blocking) ---

void LedManager::setSolid(uint8_t r, uint8_t g, uint8_t b) {
    _pixels.setPixelColor(0, _pixels.Color(r, g, b));
    _pixels.show();
}

void LedManager::runBlink(uint8_t r, uint8_t g, uint8_t b, int interval) {
    if (millis() - _lastUpdate > interval) {
        _lastUpdate = millis();
        _blinkState = !_blinkState;
        if (_blinkState) _pixels.setPixelColor(0, _pixels.Color(r, g, b));
        else _pixels.setPixelColor(0, 0);
        _pixels.show();
    }
}

void LedManager::runStrobe(uint8_t r, uint8_t g, uint8_t b) {
    // Очень быстрое мигание (20ms ON, 80ms OFF)
    uint32_t cycle = millis() % 100;
    if (cycle < 20) _pixels.setPixelColor(0, _pixels.Color(r, g, b));
    else _pixels.setPixelColor(0, 0);
    _pixels.show();
}

void LedManager::runBreathe(uint8_t r, uint8_t g, uint8_t b) {
    // Синусоидальное дыхание
    if (millis() - _lastUpdate > 20) { // 50 FPS
        _lastUpdate = millis();
        float val = (exp(sin(millis()/2000.0*PI)) - 0.36787944)*108.0;
        // Применяем яркость к базовому цвету
        uint8_t r_val = (r * (int)val) / 255;
        uint8_t g_val = (g * (int)val) / 255;
        uint8_t b_val = (b * (int)val) / 255;
        _pixels.setPixelColor(0, _pixels.Color(r_val, g_val, b_val));
        _pixels.show();
    }
}

void LedManager::runRainbow() {
    if (millis() - _lastUpdate > 20) {
        _lastUpdate = millis();
        _animStep++;
        _pixels.setPixelColor(0, Wheel(_animStep & 255));
        _pixels.show();
    }
}

uint32_t LedManager::Wheel(byte WheelPos) {
    WheelPos = 255 - WheelPos;
    if(WheelPos < 85) {
        return _pixels.Color(255 - WheelPos * 3, 0, WheelPos * 3);
    }
    if(WheelPos < 170) {
        WheelPos -= 85;
        return _pixels.Color(0, WheelPos * 3, 255 - WheelPos * 3);
    }
    WheelPos -= 170;
    return _pixels.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}