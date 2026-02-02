#include <U8g2lib.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "Config.h"
#include "SdManager.h" // Нужно для проверки статуса записи (REC)

// Константы верстки
static const int ROW_HEIGHT = 12;
static const int HEADER_HEIGHT = 12;
static const int STATUS_BAR_HEIGHT = 10;
static const int VISIBLE_ITEMS = 4;

DisplayManager& DisplayManager::getInstance() { 
    static DisplayManager i; 
    return i; 
}

DisplayManager::DisplayManager() : display(U8G2_R0, Config::PIN_OLED_SCL, Config::PIN_OLED_SDA, U8X8_PIN_NONE), _menuScrollOffset(0) {
    _currentStatus.state = SystemState::IDLE; 
    _currentStatus.logMsg[0] = '\0';
}

void DisplayManager::init() { 
    display.begin(); 
    display.setFont(u8g2_font_6x10_tf); // Экономичный, но читаемый шрифт
    display.setContrast(255); 
}

void DisplayManager::handleInput(InputEvent evt) {
    if (evt == InputEvent::NONE) return;
    _isDirty = true;

    // --- БЛОКИРОВКА ПРИЗРАЧНОГО УПРАВЛЕНИЯ ---
    // Разрешаем навигацию только если пользователь видит меню или список.
    bool isMenuVisible = (_currentStatus.state == SystemState::IDLE);
    bool isListVisible = (_currentStatus.state == SystemState::SCAN_COMPLETE);
    bool isSubMenuVisible = (_currentStatus.state == SystemState::MENU_SELECT_BLE || _currentStatus.state == SystemState::MENU_SELECT_NRF);

    if (!isMenuVisible && !isListVisible && !isSubMenuVisible) {
        return; // Игнорируем нажатия (кроме Back, который обрабатывается в System.cpp)
    }

    // --- ГЛАВНОЕ МЕНЮ ---
    if (_currentStatus.state == SystemState::IDLE) {
        int total = _menuItems.size();
        
        if (evt == InputEvent::BTN_DOWN) { 
            _menuIndex++; 
            if (_menuIndex >= total) _menuIndex = 0; 
        }
        else if (evt == InputEvent::BTN_UP) { 
            _menuIndex--; 
            if (_menuIndex < 0) _menuIndex = total - 1; 
        }

        // Smart Scrolling: Сдвиг окна просмотра
        if (_menuIndex >= _menuScrollOffset + VISIBLE_ITEMS) {
            _menuScrollOffset = _menuIndex - VISIBLE_ITEMS + 1;
        }
        else if (_menuIndex < _menuScrollOffset) {
            _menuScrollOffset = _menuIndex;
        }
    }
    // --- СПИСОК ЦЕЛЕЙ WIFI ---
    else if (_currentStatus.state == SystemState::SCAN_COMPLETE) {
         if (!_scanResults.empty()) {
             if (evt == InputEvent::BTN_DOWN) { 
                 _targetIndex++; 
                 if (_targetIndex >= (int)_scanResults.size()) _targetIndex = 0; 
             }
             else if (evt == InputEvent::BTN_UP) { 
                 _targetIndex--; 
                 if (_targetIndex < 0) _targetIndex = (int)_scanResults.size() - 1; 
             }
         }
    }
    // --- ПОДМЕНЮ BLE ---
    else if (_currentStatus.state == SystemState::MENU_SELECT_BLE) {
        if (evt == InputEvent::BTN_DOWN) _submenuIndex = (_submenuIndex + 1) % 3;
        else if (evt == InputEvent::BTN_UP) { 
            _submenuIndex--; 
            if (_submenuIndex < 0) _submenuIndex = 2; 
        }
    } 
    // --- ПОДМЕНЮ NRF ---
    else if (_currentStatus.state == SystemState::MENU_SELECT_NRF) {
        if (evt == InputEvent::BTN_DOWN) _submenuIndex = (_submenuIndex + 1) % 15;
        else if (evt == InputEvent::BTN_UP) { 
            _submenuIndex--; 
            if (_submenuIndex < 0) _submenuIndex = 14; 
        }
    }
}

void DisplayManager::render() {
    // FIX: Throttle Display if Recording (Оптимизация SPI)
    // Если SD карта занята записью, снижаем FPS экрана до 5, чтобы не вешать шину SPI
    static uint32_t lastRender = 0;
    int interval = SdManager::getInstance().isCapturing() ? 200 : 33; 
    
    if (!_isDirty && (millis() - lastRender < interval)) return;
    
    lastRender = millis();
    display.clearBuffer();
    
    // Статус бар рисуется всегда
    drawStatusBar();
    
    // Отрисовка контента
    switch (_currentStatus.state) {
        case SystemState::IDLE: 
            drawMenu(); 
            break;
            
        case SystemState::SCAN_COMPLETE: 
            drawTargetList(); 
            break;
            
        case SystemState::SCAN_EMPTY: 
            drawPopup("No Networks Found"); 
            break;
        
        // Режимы с графиками
        case SystemState::ANALYZING_NRF: 
        case SystemState::ANALYZING_SUBGHZ_RX: 
        case SystemState::SNIFFING_NRF: 
            drawSpectrum(); 
            break;
            
        // Активные атаки с поп-апами
        case SystemState::ATTACKING_SUBGHZ_TX: 
            if(_currentStatus.isReplaying) drawPopup("Replaying Signal..."); 
            else drawPopup("Jamming 433MHz..."); 
            break;
            
        // Подменю выбора
        case SystemState::MENU_SELECT_BLE: 
            drawBleMenu(); 
            break;
            
        case SystemState::MENU_SELECT_NRF: 
            drawNrfMenu(); 
            break;
        
        // Веб-интерфейс
        case SystemState::ADMIN_MODE: 
        case SystemState::WEB_CLIENT_CONNECTED: 
            drawAdminScreen(); 
            break;
            
        // По умолчанию (логи атаки: WiFi Deauth, Beacon Spam и т.д.)
        default: 
            drawAttackDetails(); 
            break;
    }
    
    display.sendBuffer(); 
    _isDirty = false;
}

void DisplayManager::drawStatusBar() {
    // Линия разделителя
    display.setDrawColor(1);
    display.drawLine(0, STATUS_BAR_HEIGHT, 128, STATUS_BAR_HEIGHT);
    
    // --- ФИЛЬТР БАТАРЕИ ---
    int raw = analogRead(Config::PIN_BAT_ADC);
    
    // Инициализация фильтра при первом запуске
    if (!_batteryInit) {
        _batteryFilterAccum = raw * 16; 
        _batteryInit = true;
    }
    
    // Exponential Moving Average (EMA) для плавности
    _batteryFilterAccum = (_batteryFilterAccum * 15 + (raw * 16)) / 16;
    int smoothedVal = _batteryFilterAccum / 16;
    
    // Map: 1860 (~3.0V) to 2600 (~4.2V)
    int pct = map(smoothedVal, 1860, 2600, 0, 100); 
    if(pct > 100) pct = 100; 
    if(pct < 0) pct = 0;
    
    // Рисуем иконку батареи
    display.drawFrame(110, 0, 16, 8); 
    display.drawBox(112, 2, (pct * 12) / 100, 4);
    
    // --- ИНДИКАТОР ЗАПИСИ (REC) ---
    // Если идет запись на SD, рисуем красный круг
    if (SdManager::getInstance().isCapturing()) {
        display.drawDisc(100, 4, 3, U8G2_DRAW_ALL); 
    }

    // Текст статуса
    const char* s = "IDLE";
    if(_currentStatus.state == SystemState::ATTACKING_WIFI_DEAUTH) s="DEAUTH";
    else if(_currentStatus.state == SystemState::ATTACKING_WIFI_SPAM) s="BEACON";
    else if(_currentStatus.state == SystemState::ATTACKING_EVIL_TWIN) s="EVIL TWIN";
    else if(_currentStatus.state == SystemState::ATTACKING_NRF) s="NRF JAM";
    else if(_currentStatus.state == SystemState::ADMIN_MODE) s="ADMIN WEB";
    else if(_currentStatus.state == SystemState::WEB_CLIENT_CONNECTED) s="WEB BUSY";
    else if(_currentStatus.state == SystemState::SCANNING) s="SCANNING";
    else if(_currentStatus.state == SystemState::ATTACKING_BLE) s="BLE SPOOF";
    else if(_currentStatus.state == SystemState::ANALYZING_SUBGHZ_RX) s="SUB-RX";
    else if(_currentStatus.state == SystemState::ATTACKING_SUBGHZ_TX) s="SUB-TX";
    
    display.setFont(u8g2_font_5x8_tf);
    display.drawStr(2, 7, s); 
    display.setFont(u8g2_font_6x10_tf); // Возвращаем нормальный шрифт
}

void DisplayManager::drawMenu() {
    int yStart = HEADER_HEIGHT + 2;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = _menuScrollOffset + i;
        if (idx >= (int)_menuItems.size()) break;
        
        int y = yStart + (i * ROW_HEIGHT);
        
        // Выделение текущего пункта (Инверсия цвета)
        if (idx == _menuIndex) {
            display.setDrawColor(1); 
            display.drawBox(0, y, 128, ROW_HEIGHT); 
            display.setDrawColor(0); // Текст станет "прозрачным"
        } else { 
            display.setDrawColor(1); 
        }
        
        display.drawStr(4, y + 9, _menuItems[idx].c_str());
    }
    display.setDrawColor(1); // Сброс цвета
}

void DisplayManager::drawTargetList() {
    if (_scanResults.empty()) { 
        display.drawStr(10, 30, "Empty List"); 
        return; 
    }
    
    // Логика прокрутки для списка целей
    int visible = 4;
    int startIdx = 0;
    if (_targetIndex >= visible) startIdx = _targetIndex - (visible - 1);
    
    for (int i = 0; i < visible; i++) {
        int idx = startIdx + i; 
        if (idx >= (int)_scanResults.size()) break;
        
        int y = 14 + (i * 12);
        
        char buf[64]; 
        char ssidSafe[22];
        
        // --- УМНАЯ ОБРЕЗКА (ELLIPSIS) ---
        int len = strlen(_scanResults[idx].ssid);
        if (len > 18) {
            strncpy(ssidSafe, _scanResults[idx].ssid, 15);
            ssidSafe[15] = 0;
            strcat(ssidSafe, "..."); // Добавляем многоточие
        } else {
            strcpy(ssidSafe, _scanResults[idx].ssid);
        }
        
        snprintf(buf, sizeof(buf), "%s %d", ssidSafe, _scanResults[idx].rssi);
        
        if (idx == _targetIndex) { 
            display.drawStr(0, y+9, ">"); 
            display.drawStr(10, y+9, buf); 
        } else {
            display.drawStr(10, y+9, buf);
        }
    }
}

void DisplayManager::drawSpectrum() { 
    // Подписи частот
    display.setFont(u8g2_font_4x6_tf);
    display.drawStr(0, 64, "433.0");
    display.drawStr(100, 64, "434.2");
    
    // Отрисовка графика спектра
    for(int x=0; x<128; x++) {
        uint8_t val = _currentStatus.spectrum[x];
        if(val > 0) {
            int h = val / 2; 
            if (h > 50) h = 50; 
            
            int yTop = 56 - h; 
            if (yTop < 12) yTop = 12; // Не залезать на статус бар
            
            display.drawLine(x, 56, x, yTop);
        }
    }
    display.setFont(u8g2_font_6x10_tf);
}

void DisplayManager::drawAttackDetails() { 
    // Основной лог атаки
    display.drawStr(0, 30, _currentStatus.logMsg); 
    
    // Иконка Rolling Code
    if(_currentStatus.rollingCodeDetected) {
        display.setFont(u8g2_font_open_iconic_check_2x_t); 
        display.drawGlyph(56, 55, 0x42); // Иконка
        display.setFont(u8g2_font_6x10_tf);
        display.drawStr(20, 60, "ROLLING CODE"); 
    }
    
    // Уведомление о хендшейке
    if(_currentStatus.handshakeCaptured) {
        display.drawStr(20, 60, "HANDSHAKE!"); 
    }
}

void DisplayManager::drawBleMenu() {
    const char* items[] = {"iOS (AirPods)", "Android (Fast)", "Win (Swift)"}; 
    display.drawStr(0, 22, "Spoof Target:");
    
    for(int i=0; i<3; i++) { 
        int y = 34 + (i*12); 
        if(i == _submenuIndex) { 
            display.drawStr(0, y, ">"); 
            display.drawStr(10, y, items[i]); 
        } 
        else {
            display.drawStr(10, y, items[i]); 
        }
    }
}

void DisplayManager::drawNrfMenu() { 
    char buf[32]; 
    snprintf(buf, 32, "Jam Channel: %d", _submenuIndex + 1); 
    display.drawStr(10, 40, buf); 
    display.drawStr(10, 55, "< Select >");
}

void DisplayManager::drawPopup(const char* msg) { 
    // Очистка
    display.setDrawColor(0); 
    display.drawBox(10, 20, 108, 30);
    display.setDrawColor(1);
    
    // Рамка
    display.drawFrame(10, 20, 108, 30);
    display.drawBox(10, 20, 108, 12); // Заголовок
    
    display.setDrawColor(0); 
    display.drawStr(45, 30, "INFO");
    
    display.setDrawColor(1); 
    display.drawStr(15, 45, msg);
}

void DisplayManager::drawAdminScreen() {
    display.setFont(u8g2_font_5x8_tf);
    display.drawStr(0, 25, "SSID: nRF_Admin");
    display.drawStr(0, 35, "IP:   192.168.4.1");
    display.drawStr(0, 50, "Status: Active");
    
    if (_currentStatus.state == SystemState::WEB_CLIENT_CONNECTED) {
        display.drawStr(0, 60, "Client: Connected");
    } else {
        display.drawStr(0, 60, "Client: Waiting...");
    }
    display.setFont(u8g2_font_6x10_tf);
}

void DisplayManager::updateStatus(const StatusMessage& msg) { 
    _currentStatus = msg; 
    _isDirty = true; 
}

void DisplayManager::showSplashScreen() { 
    display.clearBuffer(); 
    display.setFont(u8g2_font_ncenB10_tr); 
    display.drawStr(15,35,"nRF Ghost"); 
    display.setFont(u8g2_font_6x10_tf); 
    display.drawStr(40,50,"v6.1"); 
    display.sendBuffer(); 
    _isDirty = true; 
}

void DisplayManager::setTargetList(const std::vector<TargetAP>& list) { 
    _scanResults = list; 
    _isDirty = true; 
}

void DisplayManager::resetSubmenuIndex() { _submenuIndex = 0; }
int DisplayManager::getMenuIndex() const { return _menuIndex; }
int DisplayManager::getTargetIndex() const { return _targetIndex; }
int DisplayManager::getSubmenuIndex() const { return _submenuIndex; }