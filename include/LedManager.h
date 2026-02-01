#pragma once
#include "Common.h"
#include "Config.h"
#include <Adafruit_NeoPixel.h>

class LedManager {
public:
    static LedManager& getInstance();
    void init();
    void updateWithStatus(const StatusMessage& msg);
private:
    LedManager();
    Adafruit_NeoPixel _pixels;
    SystemState _lastState;
};