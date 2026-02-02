#include "InputManager.h"
#include "Config.h"

InputManager& InputManager::getInstance() { static InputManager i; return i; }

void InputManager::init() {
    pinMode(Config::PIN_BTN_UP, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_DOWN, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_SELECT, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_LEFT, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_RIGHT, INPUT_PULLUP);
}

InputEvent InputManager::poll() {
    uint32_t now = millis();
    int currentBtn = -1;

    if (digitalRead(Config::PIN_BTN_SELECT) == LOW) currentBtn = 0;
    else if (digitalRead(Config::PIN_BTN_UP) == LOW) currentBtn = 1;
    else if (digitalRead(Config::PIN_BTN_DOWN) == LOW) currentBtn = 2;
    else if (digitalRead(Config::PIN_BTN_LEFT) == LOW) currentBtn = 3;
    else if (digitalRead(Config::PIN_BTN_RIGHT) == LOW) currentBtn = 4;

    if (currentBtn != -1) {
        if (_lastBtnState != currentBtn) {
            // New Press
            if (now - _lastPressTime > DEBOUNCE_MS) {
                _lastPressTime = now;
                _btnDownTime = now;
                _lastBtnState = currentBtn;
                _holding = false;
                
                if (currentBtn == 0) return InputEvent::BTN_SELECT;
                if (currentBtn == 1) return InputEvent::BTN_UP;
                if (currentBtn == 2) return InputEvent::BTN_DOWN;
                if (currentBtn == 3) return InputEvent::BTN_BACK;
            }
        } else {
            // Holding Logic
            if (!_holding && (now - _btnDownTime > HOLD_TIME_MS)) { _holding = true; }
            
            if (_holding && (now - _lastPressTime > REPEAT_RATE_MS)) {
                _lastPressTime = now;
                if (currentBtn == 1) return InputEvent::BTN_UP;
                if (currentBtn == 2) return InputEvent::BTN_DOWN;
            }
        }
    } else {
        _lastBtnState = -1;
        _holding = false;
    }
    return InputEvent::NONE;
}