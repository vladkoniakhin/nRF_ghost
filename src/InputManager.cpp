#include "InputManager.h"
#include "Config.h"

InputManager& InputManager::getInstance() { 
    static InputManager i; 
    return i; 
}

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

    // Опрос кнопок (Low = Pressed). Приоритет не важен, т.к. нажимается одна за раз.
    if (digitalRead(Config::PIN_BTN_SELECT) == LOW) currentBtn = 0;
    else if (digitalRead(Config::PIN_BTN_UP) == LOW) currentBtn = 1;
    else if (digitalRead(Config::PIN_BTN_DOWN) == LOW) currentBtn = 2;
    else if (digitalRead(Config::PIN_BTN_LEFT) == LOW) currentBtn = 3;
    else if (digitalRead(Config::PIN_BTN_RIGHT) == LOW) currentBtn = 4;

    // Логика конечного автомата
    if (currentBtn != -1) {
        // Кнопка сейчас нажата
        
        if (_lastBtnState != currentBtn) {
            // Edge Detection: Кнопка только что нажата (из состояния отпущена)
            if (now - _lastPressTime > DEBOUNCE_MS) {
                _lastPressTime = now;
                _btnDownTime = now;
                _lastBtnState = currentBtn;
                _holding = false;
                
                // Возвращаем событие одиночного нажатия
                if (currentBtn == 0) return InputEvent::BTN_SELECT;
                if (currentBtn == 1) return InputEvent::BTN_UP;
                if (currentBtn == 2) return InputEvent::BTN_DOWN;
                if (currentBtn == 3) return InputEvent::BTN_BACK;
                // RIGHT пока не используется в меню, но зарезервирована
            }
        } else {
            // Кнопка удерживается (Holding)
            
            // Если держим дольше HOLD_TIME_MS, включаем режим удержания
            if (!_holding && (now - _btnDownTime > HOLD_TIME_MS)) {
                _holding = true; 
            }
            
            // В режиме удержания генерируем события каждые REPEAT_RATE_MS
            if (_holding && (now - _lastPressTime > REPEAT_RATE_MS)) {
                _lastPressTime = now;
                
                // Автоповтор имеет смысл только для навигации (UP/DOWN)
                if (currentBtn == 1) return InputEvent::BTN_UP;
                if (currentBtn == 2) return InputEvent::BTN_DOWN;
            }
        }
    } else {
        // Кнопка отпущена
        _lastBtnState = -1;
        _holding = false;
    }

    return InputEvent::NONE;
}