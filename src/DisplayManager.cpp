#include <U8g2lib.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "Config.h"
#include "Assets.h"

static const int ROW_HEIGHT = 12;
static const int HEADER_HEIGHT = 12;
static const int VISIBLE_ITEMS = 4;

DisplayManager& DisplayManager::getInstance() { static DisplayManager i; return i; }

DisplayManager::DisplayManager() : display(U8G2_R0, Config::PIN_OLED_SCL, Config::PIN_OLED_SDA, U8X8_PIN_NONE), _menuScrollOffset(0) {
    _currentStatus.state = SystemState::IDLE; _currentStatus.logMsg[0] = '\0';
}

void DisplayManager::init() { display.begin(); display.setFont(u8g2_font_6x10_tf); display.setContrast(255); }

void DisplayManager::handleInput(InputEvent evt) {
    if (evt == InputEvent::NONE) return;
    _isDirty = true;

    if (_currentStatus.state == SystemState::IDLE) {
        int total = _menuItems.size();
        if (evt == InputEvent::BTN_DOWN) { _menuIndex++; if (_menuIndex >= total) _menuIndex = 0; }
        else if (evt == InputEvent::BTN_UP) { _menuIndex--; if (_menuIndex < 0) _menuIndex = total - 1; }

        if (_menuIndex >= _menuScrollOffset + VISIBLE_ITEMS) _menuScrollOffset = _menuIndex - VISIBLE_ITEMS + 1;
        else if (_menuIndex < _menuScrollOffset) _menuScrollOffset = _menuIndex;
        if (_menuIndex == 0) _menuScrollOffset = 0;
        if (_menuIndex == total - 1) _menuScrollOffset = total - VISIBLE_ITEMS;
    }
    else if (_currentStatus.state == SystemState::SCAN_COMPLETE) {
         if (evt == InputEvent::BTN_DOWN) { _targetIndex++; if (_targetIndex >= (int)_scanResults.size()) _targetIndex = 0; }
         else if (evt == InputEvent::BTN_UP) { _targetIndex--; if (_targetIndex < 0) _targetIndex = (int)_scanResults.size() - 1; }
    }
    else if (_currentStatus.state == SystemState::MENU_SELECT_BLE) {
        if (evt == InputEvent::BTN_DOWN) _submenuIndex = (_submenuIndex + 1) % 3;
        else if (evt == InputEvent::BTN_UP) { _submenuIndex--; if (_submenuIndex < 0) _submenuIndex = 2; }
    } else if (_currentStatus.state == SystemState::MENU_SELECT_NRF) {
        if (evt == InputEvent::BTN_DOWN) _submenuIndex = (_submenuIndex + 1) % 15;
        else if (evt == InputEvent::BTN_UP) { _submenuIndex--; if (_submenuIndex < 0) _submenuIndex = 14; }
    }
}

void DisplayManager::render() {
    if (!_isDirty) return;
    display.clearBuffer();
    drawStatusBar();
    
    switch (_currentStatus.state) {
        case SystemState::IDLE: 
            drawMenu(); 
            break;
            
        case SystemState::SCAN_COMPLETE: 
            drawTargetList(); 
            break;
            
        case SystemState::SCAN_EMPTY: 
            drawPopup("No Networks"); 
            break;
            
        // --- NRF / SubGHz Analysis ---
        case SystemState::ANALYZING_NRF: 
        case SystemState::ANALYZING_SUBGHZ_RX: // FIXED NAME
        case SystemState::SNIFFING_NRF: 
            drawSpectrum(); 
            break;
            
        // --- SubGHz Attack ---
        case SystemState::ATTACKING_SUBGHZ_TX: // FIXED NAME
            if(_currentStatus.isReplaying) drawPopup("Replaying..."); 
            else drawPopup("Jamming 433..."); 
            break;
            
        // --- Menus ---
        case SystemState::MENU_SELECT_BLE: 
            drawBleMenu(); 
            break;
            
        case SystemState::MENU_SELECT_NRF: 
            drawNrfMenu(); 
            break;
            
        // --- Admin ---
        case SystemState::ADMIN_MODE: 
        case SystemState::WEB_CLIENT_CONNECTED: // FIXED: Handle new state
            drawAdminScreen(); 
            break;
            
        default: 
            drawAttackDetails(); 
            break;
    }
    display.sendBuffer(); _isDirty = false;
}

void DisplayManager::drawMenu() {
    int yStart = HEADER_HEIGHT + 2;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = _menuScrollOffset + i;
        if (idx >= (int)_menuItems.size()) break;
        int y = yStart + (i * ROW_HEIGHT) + 8;
        if (idx == _menuIndex) {
            display.setDrawColor(1); display.drawBox(0, yStart + (i*ROW_HEIGHT), 128, ROW_HEIGHT); display.setDrawColor(0);
        } else { display.setDrawColor(1); }
        display.drawStr(10, y, _menuItems[idx].c_str());
    }
    display.setDrawColor(1);
}

void DisplayManager::drawStatusBar() {
    display.drawLine(0, 10, 128, 10);
    int adc = analogRead(Config::PIN_BAT_ADC); int pct = map(adc, 1860, 2600, 0, 100); if(pct>100) pct=100; if(pct<0) pct=0;
    display.drawFrame(110, 0, 14, 8); display.drawBox(112, 2, pct/10, 4);
    
    const char* s = "IDLE";
    
    // FIXED: Updated Enum checks
    if(_currentStatus.state == SystemState::ATTACKING_WIFI_DEAUTH) s="DEAUTH";
    else if(_currentStatus.state == SystemState::ATTACKING_WIFI_SPAM) s="BEACON";
    else if(_currentStatus.state == SystemState::ATTACKING_EVIL_TWIN) s="EVIL TWIN";
    else if(_currentStatus.state == SystemState::ATTACKING_NRF) s="NRF JAM";
    else if(_currentStatus.state == SystemState::ADMIN_MODE) s="ADMIN";
    else if(_currentStatus.state == SystemState::WEB_CLIENT_CONNECTED) s="WEB CON";
    
    display.drawStr(0, 8, s); 
}

void DisplayManager::drawTargetList() {
    if (_scanResults.empty()) { display.drawStr(0, 30, "Empty List"); return; }
    int startIdx = (_targetIndex > 3) ? _targetIndex - 3 : 0;
    for (int i = 0; i < 4; i++) {
        int idx = startIdx + i; if (idx >= (int)_scanResults.size()) break;
        int y = 24 + (i * 10);
        char buf[32]; snprintf(buf, 32, "%s (%d)", _scanResults[idx].ssid, _scanResults[idx].rssi);
        if (idx == _targetIndex) { display.drawStr(0, y, ">"); display.drawStr(10, y, buf); } else display.drawStr(10, y, buf);
    }
}

void DisplayManager::drawBleMenu() {
    const char* items[] = {"iOS", "Android", "Windows"}; display.drawStr(0, 20, "Select Target:");
    for(int i=0; i<3; i++) { int y = 30 + (i*12); if(i == _submenuIndex) { display.drawStr(0, y, ">"); display.drawStr(10, y, items[i]); } else display.drawStr(10, y, items[i]); }
}
void DisplayManager::drawNrfMenu() { char buf[20]; snprintf(buf, 20, "Ch: %d", _submenuIndex + 1); display.drawStr(40, 40, buf); }
void DisplayManager::drawPopup(const char* msg) { display.drawStr(0,30,msg); }
void DisplayManager::drawSpectrum() { for(int x=0; x<128; x++) if(_currentStatus.spectrum[x] > 0) display.drawLine(x, 63, x, 63 - (_currentStatus.spectrum[x]/2)); }
void DisplayManager::drawAttackDetails() { 
    display.drawStr(0,30,_currentStatus.logMsg); 
    if(_currentStatus.rollingCodeDetected) display.drawStr(0,50,"! ROLLING CODE !"); 
}

void DisplayManager::drawAdminScreen() {
    display.setFont(u8g2_font_5x8_tf);
    display.drawStr(0, 25, "SSID: nRF_Admin");
    display.drawStr(0, 35, "IP:   192.168.4.1");
    display.drawStr(0, 50, "Connect & Open Browser");
    display.setFont(u8g2_font_6x10_tf);
}

void DisplayManager::updateStatus(const StatusMessage& msg) { _currentStatus = msg; _isDirty = true; }
void DisplayManager::showSplashScreen() { display.clearBuffer(); display.drawStr(30,30,"nRF Ghost v3"); display.sendBuffer(); _isDirty=true; }
void DisplayManager::setTargetList(const std::vector<TargetAP>& list) { _scanResults = list; _isDirty = true; }