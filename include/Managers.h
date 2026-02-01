#pragma once
#include "Common.h"
#include <U8g2lib.h>
#include <vector>
#include <string>

// ---------------------------------------------------------
// Input Manager: Handles Debouncing & Event Translation
// ---------------------------------------------------------
class InputManager {
public:
    static InputManager& getInstance();
    
    void init();
    InputEvent poll(); // Call this every loop in UI Task

private:
    InputManager() = default;
    InputManager(const InputManager&) = delete;
    
    // Config
    const uint32_t DEBOUNCE_MS = 200;
    uint32_t _lastPressTime = 0;
};

// ---------------------------------------------------------
// Display Manager: Handles OLED & Dirty Bit Rendering
// ---------------------------------------------------------
class DisplayManager {
public:
    static DisplayManager& getInstance();

    void init();
    
    // The main render loop (Call 60 times a second max)
    void render();

    // Updates internal state for menu navigation
    void handleInput(InputEvent evt);

    // Setters that trigger the Dirty Bit
    void updateStatus(const StatusMessage& msg);
    void showSplashScreen();

private:
    DisplayManager();
    DisplayManager(const DisplayManager&) = delete;
    
    // Use the Full Frame Buffer constructor for speed on ESP32
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C display;
    
    // State
    bool _isDirty = true; 
    int _menuIndex = 0;
    StatusMessage _currentStatus;

    // Menu Definitions
    const std::vector<std::string> _menuItems = {
        "WiFi Scan",
        "WiFi Deauth",
        "BLE Spoofer",
        "NRF Jammer",
        "Stop All"
    };

    // Internal helpers
    void drawStatusBar();
    void drawMenu();
    void drawAttackDetails();
};