#pragma once
#include "Common.h"

class InputManager {
public:
    static InputManager& getInstance();
    
    void init();
    InputEvent poll(); 

private:
    InputManager() = default;
    InputManager(const InputManager&) = delete;
    void operator=(const InputManager&) = delete;
    
    // Config: Быстрый отклик + Автоповтор
    const uint32_t DEBOUNCE_MS = 50;       // Уменьшено с 200 до 50 для отзывчивости
    const uint32_t HOLD_TIME_MS = 500;     // Время удержания до начала автоповтора
    const uint32_t REPEAT_RATE_MS = 100;   // Скорость прокрутки при удержании

    uint32_t _lastPressTime = 0;
    uint32_t _btnDownTime = 0;
    int _lastBtnState = -1; // -1: None
    bool _holding = false;
};