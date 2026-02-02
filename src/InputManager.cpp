#include "InputManager.h"
#include "Config.h"
#include <queue> // Needed for buffering

static std::queue<InputEvent> g_inputBuffer;

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
            if (!_holding && (now - _btnDownTime > HOLD_TIME_MS)) _holding = true;
            if (_holding && (now - _lastPressTime > REPEAT_RATE_MS)) {
                _lastPressTime = now;
                if (currentBtn == 1) g_inputBuffer.push(InputEvent::BTN_UP);
                if (currentBtn == 2) g_inputBuffer.push(InputEvent::BTN_DOWN);
            }
        }
    } else {
        _lastBtnState = -1;
        _holding = false;
    }

    // Возвращаем события из буфера по одному
    if (!g_inputBuffer.empty()) {
        InputEvent e = g_inputBuffer.front();
        g_inputBuffer.pop();
        return e;
    }

    return InputEvent::NONE;
}