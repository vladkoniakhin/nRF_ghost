#include "InputManager.h"
#include "Config.h"
#include <queue>

static std::queue<InputEvent> g_inputBuffer;

InputManager& InputManager::getInstance() { static InputManager i; return i; }

void InputManager::init() {
    pinMode(Config::PIN_BTN_UP, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_DOWN, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_SELECT, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_LEFT, INPUT_PULLUP); // BACK
    // Right button init removed (GPIO 2 freed)
}

InputEvent InputManager::poll() {
    uint32_t now = millis();
    int currentBtn = -1;

    // Polling 4 buttons (Low = Pressed)
    if (digitalRead(Config::PIN_BTN_SELECT) == LOW) currentBtn = 0;
    else if (digitalRead(Config::PIN_BTN_UP) == LOW) currentBtn = 1;
    else if (digitalRead(Config::PIN_BTN_DOWN) == LOW) currentBtn = 2;
    else if (digitalRead(Config::PIN_BTN_LEFT) == LOW) currentBtn = 3;

    if (currentBtn != -1) {
        // Button Pressed
        if (_lastBtnState != currentBtn) {
            // Edge Detection (New Press)
            if (now - _lastPressTime > DEBOUNCE_MS) {
                _lastPressTime = now;
                _btnDownTime = now;
                _lastBtnState = currentBtn;
                _holding = false;
                
                InputEvent e = InputEvent::NONE;
                if (currentBtn == 0) e = InputEvent::BTN_SELECT;
                if (currentBtn == 1) e = InputEvent::BTN_UP;
                if (currentBtn == 2) e = InputEvent::BTN_DOWN;
                if (currentBtn == 3) e = InputEvent::BTN_BACK;
                
                if (e != InputEvent::NONE) g_inputBuffer.push(e);
            }
        } else {
            // Holding Logic (Auto-repeat for UP/DOWN)
            if (!_holding && (now - _btnDownTime > HOLD_TIME_MS)) {
                _holding = true; 
            }
            
            if (_holding && (now - _lastPressTime > REPEAT_RATE_MS)) {
                _lastPressTime = now;
                // Only auto-repeat navigation keys
                if (currentBtn == 1) g_inputBuffer.push(InputEvent::BTN_UP);
                if (currentBtn == 2) g_inputBuffer.push(InputEvent::BTN_DOWN);
            }
        }
    } else {
        // Button Released
        _lastBtnState = -1;
        _holding = false;
    }

    // Return events from buffer one by one
    if (!g_inputBuffer.empty()) {
        InputEvent e = g_inputBuffer.front();
        g_inputBuffer.pop();
        return e;
    }

    return InputEvent::NONE;
}