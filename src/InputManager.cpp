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
    if (now - _lastPressTime < DEBOUNCE_MS) return InputEvent::NONE;
    if (digitalRead(Config::PIN_BTN_SELECT) == LOW) { _lastPressTime = now; return InputEvent::BTN_SELECT; }
    if (digitalRead(Config::PIN_BTN_UP) == LOW) { _lastPressTime = now; return InputEvent::BTN_UP; }
    if (digitalRead(Config::PIN_BTN_DOWN) == LOW) { _lastPressTime = now; return InputEvent::BTN_DOWN; }
    if (digitalRead(Config::PIN_BTN_LEFT) == LOW) { _lastPressTime = now; return InputEvent::BTN_BACK; }
    return InputEvent::NONE;
}