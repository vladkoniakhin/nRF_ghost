#include <U8g2lib.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "Config.h"

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
    _isDirty = true; // Флаг перерисовки

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

        // Умная прокрутка (Smart Scrolling)
        // Если курсор ушел ниже видимой зоны -> сдвигаем окно вниз
        if (_menuIndex >= _menuScrollOffset + VISIBLE_ITEMS) {
            _menuScrollOffset = _menuIndex - VISIBLE_ITEMS + 1;
        }
        // Если курсор ушел выше видимой зоны -> сдвигаем окно вверх
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
        else if (evt == InputEvent::BTN_UP) { _submenuIndex--; if (_submenuIndex < 0) _submenuIndex = 2; }
    } 
    // --- ПОДМЕНЮ NRF ---
    else if (_currentStatus.state == SystemState::MENU_SELECT_NRF) {
        if (evt == InputEvent::BTN_DOWN) _submenuIndex = (_submenuIndex + 1) % 15;
        else if (evt == InputEvent::BTN_UP) { _submenuIndex--; if (_submenuIndex < 0) _submenuIndex = 14; }
    }
}

void DisplayManager::render() {
    if (!_isDirty) return;
    
    display.clearBuffer();
    
    // Отрисовка статус-бара всегда сверху
    drawStatusBar();
    
    // Основной контент (начинается с Y=12)
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
        
        // Графики и Анализ
        case SystemState::ANALYZING_NRF: 
        case SystemState::ANALYZING_SUBGHZ_RX: 
        case SystemState::SNIFFING_NRF: 
            drawSpectrum(); 
            break;
            
        // Активные Атаки
        case SystemState::ATTACKING_SUBGHZ_TX: 
            if(_currentStatus.isReplaying) drawPopup("Replaying Signal..."); 
            else drawPopup("Jamming 433MHz..."); 
            break;
            
        // Подменю
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
            
        // По умолчанию (логи атаки)
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
    
    // Индикатор батареи (Безопасный расчет)
    int adc = analogRead(Config::PIN_BAT_ADC); 
    // Примерная калибровка делителя 100к/100к: 
    // 3.0V = ~1860 ADC, 4.2V = ~2600 ADC
    int pct = map(adc, 1860, 2600, 0, 100); 
    if(pct > 100) pct = 100; 
    if(pct < 0) pct = 0;
    
    // Рисуем иконку батарейки
    display.drawFrame(110, 0, 16, 8); // Корпус
    display.drawBox(112, 2, (pct * 12) / 100, 4); // Заливка (макс ширина 12px)
    
    // Текст статуса
    const char* s = "IDLE";
    if(_currentStatus.state == SystemState::ATTACKING_WIFI_DEAUTH) s="DEAUTH";
    else if(_currentStatus.state == SystemState::ATTACKING_WIFI_SPAM) s="BEACON";
    else if(_currentStatus.state == SystemState::ATTACKING_EVIL_TWIN) s="EVIL TWIN";
    else if(_currentStatus.state == SystemState::ATTACKING_NRF) s="NRF JAM";
    else if(_currentStatus.state == SystemState::ADMIN_MODE) s="ADMIN WEB";
    else if(_currentStatus.state == SystemState::WEB_CLIENT_CONNECTED) s="WEB BUSY";
    else if(_currentStatus.state == SystemState::SCANNING) s="SCANNING";
    
    // Мелкий шрифт для статуса
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
            display.setDrawColor(0); // Текст станет "прозрачным" (черным на белом)
        } else { 
            display.setDrawColor(1); 
        }
        
        // Вертикальное центрирование текста (+9 пикселей от верха строки)
        display.drawStr(4, y + 9, _menuItems[idx].c_str());
    }
    display.setDrawColor(1); // Сброс цвета на белый
}

void DisplayManager::drawTargetList() {
    if (_scanResults.empty()) { 
        display.drawStr(10, 30, "Empty List"); 
        return; 
    }
    
    // Логика прокрутки для целей
    int visible = 4;
    int startIdx = 0;
    if (_targetIndex >= visible) startIdx = _targetIndex - (visible - 1);
    
    for (int i = 0; i < visible; i++) {
        int idx = startIdx + i; 
        if (idx >= (int)_scanResults.size()) break;
        
        int y = 14 + (i * 12);
        
        // FIX: Защита от переполнения буфера и кривой верстки
        char buf[64]; // Увеличенный буфер
        char ssidSafe[22];
        
        // Обрезаем SSID до 20 символов, чтобы влезло в экран
        strncpy(ssidSafe, _scanResults[idx].ssid, 20);
        ssidSafe[20] = 0; // Гарантируем нуль-терминатор
        
        // Форматируем строку: "SSID -85dBm"
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
    
    // Отрисовка графика
    for(int x=0; x<128; x++) {
        uint8_t val = _currentStatus.spectrum[x];
        if(val > 0) {
            // Масштабируем высоту, чтобы не вылезти за пределы (макс 50px)
            int h = val / 2; 
            if (h > 50) h = 50; 
            
            // FIX: Защита от отрицательных координат
            int yTop = 56 - h;
            if (yTop < 12) yTop = 12; // Не залезать на статус бар
            
            display.drawLine(x, 56, x, yTop);
        }
    }
    display.setFont(u8g2_font_6x10_tf);
}

void DisplayManager::drawAttackDetails() { 
    // Лог
    display.drawStr(0, 30, _currentStatus.logMsg); 
    
    // Визуальные алерты
    if(_currentStatus.rollingCodeDetected) {
        display.setFont(u8g2_font_open_iconic_check_2x_t); // Иконка галочки
        display.drawGlyph(56, 55, 0x42); 
        display.setFont(u8g2_font_6x10_tf);
        display.drawStr(20, 60, "ROLLING CODE"); 
    }
    
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
    // Рисуем рамку по центру
    display.setDrawColor(0); // Очистка под поп-апом
    display.drawBox(10, 20, 108, 30);
    display.setDrawColor(1);
    
    display.drawFrame(10, 20, 108, 30);
    display.drawBox(10, 20, 108, 12); // Заголовок
    
    display.setDrawColor(0); // Инверсный текст заголовка
    display.drawStr(45, 30, "INFO");
    
    display.setDrawColor(1); // Нормальный текст сообщения
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

void DisplayManager::updateStatus(const StatusMessage& msg) { _currentStatus = msg; _isDirty = true; }
void DisplayManager::showSplashScreen() { display.clearBuffer(); display.setFont(u8g2_font_ncenB10_tr); display.drawStr(15,35,"nRF Ghost"); display.setFont(u8g2_font_6x10_tf); display.drawStr(40,50,"v5.4"); display.sendBuffer(); _isDirty=true; }
void DisplayManager::setTargetList(const std::vector<TargetAP>& list) { _scanResults = list; _isDirty = true; }
void DisplayManager::resetSubmenuIndex() { _submenuIndex = 0; }
int DisplayManager::getMenuIndex() const { return _menuIndex; }
int DisplayManager::getTargetIndex() const { return _targetIndex; }
int DisplayManager::getSubmenuIndex() const { return _submenuIndex; }