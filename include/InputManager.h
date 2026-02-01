#pragma once
#include "Common.h"

class InputManager {
public:
    static InputManager& getInstance();
    void init();
    InputEvent poll();
private:
    InputManager() = default;
    const uint32_t DEBOUNCE_MS = 200;
    uint32_t _lastPressTime = 0;
};