#include "InputManager.h"
#include "Config.h"
#include <queue>

// Глобальная очередь
static std::queue<InputEvent> g_inputBuffer;
static const size_t MAX_QUEUE_SIZE = 5; // Защита от переполнения (Anti-DoS)

InputManager& InputManager::getInstance() { 
    static InputManager i; 
    return i; 
}

void InputManager::init() {
    pinMode(Config::PIN_BTN_UP, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_DOWN, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_SELECT, INPUT_PULLUP);
    pinMode(Config::PIN_BTN_LEFT, INPUT_PULLUP); 
}

// FIX v7.0: Очистка очереди при смене режимов
void InputManager::clear() {
    std::queue<InputEvent> empty;
    std::swap(g_inputBuffer, empty);
}

InputEvent InputManager::poll() {
    uint32_t now = millis();
    int currentBtn = -1;

    // Опрос кнопок (Low = Нажато)
    if (digitalRead(Config::PIN_BTN_SELECT) == LOW) currentBtn = 0;
    else if (digitalRead(Config::PIN_BTN_UP) == LOW) currentBtn = 1;
    else if (digitalRead(Config::PIN_BTN_DOWN) == LOW) currentBtn = 2;
    else if (digitalRead(Config::PIN_BTN_LEFT) == LOW) currentBtn = 3;

    if (currentBtn != -1) {
        if (_lastBtnState != currentBtn) {
            // Детекция нажатия (Edge)
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
                
                // FIX v7.0: Защита от переполнения очереди
                if (e != InputEvent::NONE && g_inputBuffer.size() < MAX_QUEUE_SIZE) {
                    g_inputBuffer.push(e);
                }
            }
        } else {
            // Удержание (Auto-repeat)
            if (!_holding && (now - _btnDownTime > HOLD_TIME_MS)) {
                _holding = true; 
            }
            
            if (_holding && (now - _lastPressTime > REPEAT_RATE_MS)) {
                _lastPressTime = now;
                // Автоповтор только для навигации
                if (currentBtn == 1 && g_inputBuffer.size() < MAX_QUEUE_SIZE) g_inputBuffer.push(InputEvent::BTN_UP);
                if (currentBtn == 2 && g_inputBuffer.size() < MAX_QUEUE_SIZE) g_inputBuffer.push(InputEvent::BTN_DOWN);
            }
        }
    } else {
        // Кнопка отпущена
        _lastBtnState = -1;
        _holding = false;
    }

    if (!g_inputBuffer.empty()) {
        InputEvent e = g_inputBuffer.front();
        g_inputBuffer.pop();
        return e;
    }

    return InputEvent::NONE;
}