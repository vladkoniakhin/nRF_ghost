#pragma once
#include "Common.h"
#include "Config.h"
#include <Adafruit_NeoPixel.h>

class LedManager {
public:
    static LedManager& getInstance();
    void init();
    
    // Вызывается в главном цикле UI (non-blocking)
    void update(); 
    
    void setStatus(const StatusMessage& msg);

private:
    LedManager();
    
    Adafruit_NeoPixel _pixels;
    SystemState _currentState;
    bool _handshakeCaptured;
    bool _rollingCode;
    
    // Animation State vars
    uint32_t _lastUpdate;
    uint16_t _animStep;
    bool _blinkState;
    
    // Helpers
    uint32_t Wheel(byte WheelPos);
    void setSolid(uint8_t r, uint8_t g, uint8_t b);
    void runBlink(uint8_t r, uint8_t g, uint8_t b, int interval);
    void runStrobe(uint8_t r, uint8_t g, uint8_t b);
    void runBreathe(uint8_t r, uint8_t g, uint8_t b);
    void runRainbow();
};