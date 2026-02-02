#pragma once
#include "Common.h"
#include <queue>

class InputManager {
public:
    static InputManager& getInstance();
    void init();
    InputEvent poll(); 

private:
    InputManager() = default;
    const uint32_t DEBOUNCE_MS = 50;       
    const uint32_t HOLD_TIME_MS = 500;     
    const uint32_t REPEAT_RATE_MS = 100;   

    uint32_t _lastPressTime = 0;
    uint32_t _btnDownTime = 0;
    int _lastBtnState = -1;
    bool _holding = false;
};