#pragma once
#include <U8g2lib.h>
#include "Common.h"
#include <vector>
#include <string>

class DisplayManager {
public:
    static DisplayManager& getInstance();
    void init();
    void render();
    void handleInput(InputEvent evt);
    void updateStatus(const StatusMessage& msg);
    void showSplashScreen();
    
    int getMenuIndex() const { return _menuIndex; }
    void setTargetList(const std::vector<TargetAP>& list);
    int getTargetIndex() const { return _targetIndex; }
    int getSubmenuIndex() const { return _submenuIndex; }
    void resetSubmenuIndex() { _submenuIndex = 0; }
    void drawPopup(const char* msg);

private:
    DisplayManager();
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C display;
    
    bool _isDirty = true;
    int _menuIndex = 0;
    int _menuScrollOffset = 0;
    
    int _targetIndex = 0;
    int _submenuIndex = 0;
    StatusMessage _currentStatus;
    std::vector<TargetAP> _scanResults;

    long _batteryFilterAccum = 0;
    bool _batteryInit = false;
    
    // FIX v6.2: Added "Last Scan" at index 1
    const std::vector<std::string> _menuItems = {
        "WiFi Scan", 
        "Last Scan", // New Item
        "WiFi Deauth", 
        "Beacon Spam", 
        "Evil Twin",
        "BLE Spoofer", 
        "NRF Jammer", 
        "NRF Analyzer", 
        "NRF Sniffer", 
        "MouseJack",
        "SubGhz Scan", 
        "SubGhz Jam", 
        "SubGhz RX", 
        "SubGhz TX",
        "Admin Panel", 
        "Stop All"
    };
    
    void drawStatusBar();
    void drawMenu();
    void drawTargetList();
    void drawAttackDetails();
    void drawSpectrum();
    void drawBleMenu();
    void drawNrfMenu();
    void drawAdminScreen();
};