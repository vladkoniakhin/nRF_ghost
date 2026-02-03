#include "System.h"
#include "DisplayManager.h"
#include "SdManager.h"
#include "UpdateManager.h"
#include "WiFiManager.h"
#include "BleManager.h"
#include "NrfManager.h"
#include "SubGhzManager.h"
#include "WebPortalManager.h"
#include "ConfigManager.h" 
#include "ScriptManager.h"
#include "SettingsManager.h"
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <SD.h> 
#include <WiFi.h> // Критично для управления питанием радио

// Глобальные объекты
SemaphoreHandle_t g_spiMutex = nullptr;
static char g_serialBuffer[256]; 
static uint16_t g_serialIndex = 0;

// Класс-обертка для мьютекса
class SpiLock {
public:
    SpiLock(uint32_t timeoutMs = 1000) {
        if (g_spiMutex) _acquired = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(timeoutMs));
    }
    ~SpiLock() {
        if (_acquired && g_spiMutex) xSemaphoreGive(g_spiMutex);
    }
    bool locked() const { return _acquired; }
private:
    bool _acquired = false;
};

SystemController& SystemController::getInstance() { static SystemController instance; return instance; }

SystemController::SystemController() : _currentState(SystemState::IDLE), _activeEngine(nullptr) {
    _commandQueue = xQueueCreate(10, sizeof(CommandMessage));
    _statusQueue = xQueueCreate(1, sizeof(StatusMessage));
    memset(&_selectedTarget, 0, sizeof(TargetAP));
}

void SystemController::init() {
    g_spiMutex = xSemaphoreCreateMutex();
    
    // WDT 5 секунд
    esp_task_wdt_init(5, true); 
    esp_task_wdt_add(NULL);
    
    SettingsManager::getInstance().init();
    LedManager::getInstance().init();

    // Проверка SD карты
    {
        SpiLock lock(2000);
        if (lock.locked()) {
            SdManager::getInstance().init();
            if (!SdManager::getInstance().isMounted()) {
                StatusMessage err; err.state = SystemState::SD_ERROR;
                while(true) { 
                    esp_task_wdt_reset(); 
                    LedManager::getInstance().setStatus(err); 
                    LedManager::getInstance().update(); 
                    delay(10); 
                }
            }
        }
    }

    ConfigManager::getInstance().init();
    ScriptManager::getInstance().init();
    UpdateManager::performUpdateIfAvailable();
    
    // Инициализация менеджеров (но не запуск радио)
    _wifiEngine.setup();
    BleManager::getInstance().setup();
    NrfManager::getInstance().setup();
    SubGhzManager::getInstance().setup();
    
    // Принудительно гасим WiFi на старте (Silence Mode)
    WiFi.mode(WIFI_OFF);
    
    Serial.println("[SYS] System Ready. v6.4 Hardware Hardened");
}

void SystemController::stopCurrentTask() {
    if (_activeEngine) { 
        _activeEngine->stop(); 
        _activeEngine = nullptr; 
    }
    ScriptManager::getInstance().stop();
    if (_currentState == SystemState::ADMIN_MODE) {
        WebPortalManager::getInstance().stop();
    }
    _currentState = SystemState::IDLE;
    
    // FIX v6.4: Полное отключение всех радиомодулей в простое для экономии энергии и охлаждения
    WiFi.mode(WIFI_OFF);
    NrfManager::getInstance().stop();
    SubGhzManager::getInstance().stop();
    
    Serial.println("[SYS] Stopped. Radios OFF.");
}

bool SystemController::sendCommand(CommandMessage cmd) { return xQueueSend(_commandQueue, &cmd, 0) == pdTRUE; }
bool SystemController::getStatus(StatusMessage& msg) { return xQueueReceive(_statusQueue, &msg, 0) == pdTRUE; }

// --- JSON API ---
void SystemController::sendJsonSuccess(const char* msg) { Serial.printf("{\"status\":\"ok\",\"msg\":\"%s\"}\n", msg); }
void SystemController::sendJsonError(const char* err) { Serial.printf("{\"status\":\"error\",\"msg\":\"%s\"}\n", err); }

void SystemController::sendJsonFileList(const char* path) {
    SpiLock lock(1000);
    if (lock.locked()) {
        File root = SD.open(path);
        if (!root || !root.isDirectory()) sendJsonError("Bad path");
        else {
            Serial.print("{\"files\":["); File file = root.openNextFile(); bool first = true;
            while (file) { 
                esp_task_wdt_reset(); 
                if (!first) Serial.print(","); 
                const char* name = file.name(); 
                if (name[0] != '.') { 
                    Serial.printf("{\"n\":\"%s\",\"s\":%d}", name, file.size()); 
                    first = false; 
                } 
                file = root.openNextFile(); 
            }
            Serial.println("]}");
        }
    } else sendJsonError("SPI Busy");
}

void SystemController::parseSerialJson(char* input) {
    StaticJsonDocument<256> doc; 
    DeserializationError error = deserializeJson(doc, input);
    if (error) { sendJsonError("Invalid JSON"); return; }
    
    const char* cmdStr = doc["CMD"]; 
    if (!cmdStr) return;

    if (strcmp(cmdStr, "SCAN") == 0) processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
    else if (strcmp(cmdStr, "STOP") == 0) processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
    else if (strcmp(cmdStr, "LIST") == 0) sendJsonFileList("/");
    else if (strcmp(cmdStr, "JAM") == 0) processCommand({SystemCommand::CMD_START_NRF_JAM, 40});
    else sendJsonError("Unknown command");
}

// --- WORKER LOOP ---
void SystemController::runWorkerLoop() {
    CommandMessage cmd; StatusMessage statusOut; memset(&statusOut, 0, sizeof(StatusMessage));
    uint32_t lastWsPush = 0;

    for (;;) {
        esp_task_wdt_reset();
        
        // Admin Panel Heartbeat
        if (millis() - lastWsPush > 100) { 
            if (_currentState == SystemState::ADMIN_MODE) {
                WebPortalManager::getInstance().processDns(); 
                WebPortalManager::getInstance().broadcastStatus(statusOut.logMsg, ESP.getFreeHeap());
            }
            lastWsPush = millis();
        }

        // Command Processing
        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) processCommand(cmd);

        // Serial Input
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') { 
                g_serialBuffer[g_serialIndex] = '\0'; 
                if (g_serialBuffer[0] == '{') parseSerialJson(g_serialBuffer); 
                else { 
                    if (strncmp(g_serialBuffer, "SCAN", 4) == 0) processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0}); 
                    else if (strncmp(g_serialBuffer, "STOP", 4) == 0) processCommand({SystemCommand::CMD_STOP_ATTACK, 0}); 
                } 
                g_serialIndex = 0; 
            } else { 
                if (g_serialIndex < sizeof(g_serialBuffer) - 1) g_serialBuffer[g_serialIndex++] = c; 
                else g_serialIndex = 0; 
            }
        }

        // Engine Loop
        bool running = false;
        if (_currentState == SystemState::ADMIN_MODE) { 
            statusOut.state = SystemState::ADMIN_MODE; 
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Web Admin Mode"); 
            running = true; 
        } 
        else if (_activeEngine) {
            running = _activeEngine->loop(statusOut);
            
            if (!running) {
                // Обработка завершения задач
                if (_activeEngine == &_wifiEngine && statusOut.state == SystemState::SCAN_COMPLETE) { 
                    statusOut.state = SystemState::SCAN_COMPLETE; _currentState = SystemState::SCAN_COMPLETE; 
                } 
                else if (_activeEngine == &SubGhzManager::getInstance() && statusOut.state == SystemState::ANALYZING_SUBGHZ_RX) { 
                    stopCurrentTask(); 
                    statusOut.state = SystemState::SCAN_COMPLETE; // Reuse for "Waiting"
                    _currentState = SystemState::SCAN_COMPLETE; 
                    snprintf(statusOut.logMsg, MAX_LOG_MSG, "Code Captured!"); 
                }
                else { 
                    stopCurrentTask(); 
                    statusOut.state = SystemState::IDLE; 
                    snprintf(statusOut.logMsg, MAX_LOG_MSG, "Finished"); 
                }
            }
        } else { 
            statusOut.state = (_currentState == SystemState::SCAN_COMPLETE) ? SystemState::SCAN_COMPLETE : SystemState::IDLE; 
        }
        
        xQueueOverwrite(_statusQueue, &statusOut); 
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// FIX v6.4: Универсальная функция управления питанием радио
// Это и есть то место, которое сэкономило 100 строк кода
void prepareRadio(bool wifiNeeded, bool nrfNeeded, bool subGhzNeeded) {
    if (!wifiNeeded) {
        WiFi.mode(WIFI_OFF);
    }
    if (!nrfNeeded) {
        NrfManager::getInstance().stop(); 
    }
    if (!subGhzNeeded) {
        SubGhzManager::getInstance().stop();
    }
    
    // Небольшая задержка, чтобы питание стабилизировалось после отключения мощных потребителей
    if (!wifiNeeded) vTaskDelay(10);
}

void SystemController::processCommand(CommandMessage cmd) {
    if (cmd.cmd == SystemCommand::CMD_SELECT_TARGET) {
        auto results = _wifiEngine.getScanResults();
        if (cmd.param1 >= 0 && cmd.param1 < (int)results.size()) _selectedTarget = results[cmd.param1];
        return;
    }
    if (cmd.cmd == SystemCommand::CMD_STOP_ATTACK) { stopCurrentTask(); return; }
    if (cmd.cmd == SystemCommand::CMD_SAVE_SETTINGS) { 
        bool current = SettingsManager::getInstance().getLedEnabled(); 
        SettingsManager::getInstance().setLedEnabled(!current); 
        return; 
    }

    if (cmd.cmd == SystemCommand::CMD_START_ADMIN_MODE) {
        if (_activeEngine != nullptr) return;
        prepareRadio(true, false, false); // WiFi Only
        _currentState = SystemState::ADMIN_MODE;
        WebPortalManager::getInstance().start(""); 
        return;
    }

    if (_activeEngine != nullptr) { DisplayManager::getInstance().drawPopup("Busy!"); return; }

    switch (cmd.cmd) {
        case SystemCommand::CMD_START_SCAN_WIFI: 
            prepareRadio(true, false, false);
            memset(&_selectedTarget, 0, sizeof(TargetAP)); // Reset Target
            _activeEngine = &_wifiEngine; _wifiEngine.startScan(); break;

        case SystemCommand::CMD_START_DEAUTH: 
            if (_selectedTarget.bssid[0] != 0) { 
                prepareRadio(true, false, false);
                _activeEngine = &_wifiEngine; _wifiEngine.startDeauth(_selectedTarget); 
            } else DisplayManager::getInstance().drawPopup("No Target Selected!");
            break;

        case SystemCommand::CMD_START_EVIL_TWIN: 
            if (_selectedTarget.bssid[0] != 0) { 
                prepareRadio(true, false, false);
                _activeEngine = &_wifiEngine; _wifiEngine.startEvilTwin(_selectedTarget); 
            } else DisplayManager::getInstance().drawPopup("No Target Selected!");
            break;

        case SystemCommand::CMD_START_BEACON_SPAM: 
            prepareRadio(true, false, false);
            _activeEngine = &_wifiEngine; _wifiEngine.startBeaconSpam(); break;
            
        case SystemCommand::CMD_START_BLE_SPOOF: 
            prepareRadio(true, false, false); // BLE is part of WiFi/BT Stack
            _activeEngine = &BleManager::getInstance(); BleManager::getInstance().startSpoof((BleSpoofType)cmd.param1); break;
            
        // --- HARDWARE EXCLUSIVE BLOCK ---
        
        case SystemCommand::CMD_START_NRF_JAM: 
            prepareRadio(false, true, false); // WiFi KILL
            DisplayManager::getInstance().drawPopup("WiFi Disabled");
            _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startJamming((uint8_t)cmd.param1); break;

        case SystemCommand::CMD_START_NRF_ANALYZER: 
            prepareRadio(false, true, false);
            _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startAnalyzer(); break;

        case SystemCommand::CMD_START_MOUSEJACK: 
            prepareRadio(false, true, false);
            _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startMouseJack(0); break;

        case SystemCommand::CMD_START_NRF_SNIFF: 
            prepareRadio(false, true, false);
            _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startSniffing(); break;
            
        case SystemCommand::CMD_START_SUBGHZ_SCAN: 
            prepareRadio(false, false, true); // SubGhz Only
            _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startAnalyzer(); break;

        case SystemCommand::CMD_START_SUBGHZ_JAM: 
            prepareRadio(false, false, true);
            _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startJammer(); break;

        case SystemCommand::CMD_START_SUBGHZ_RX: 
            prepareRadio(false, false, true);
            _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startCapture(); break;

        case SystemCommand::CMD_START_SUBGHZ_TX: 
            prepareRadio(false, false, true);
            _activeEngine = &SubGhzManager::getInstance(); 
            if (cmd.param1 == 1) SubGhzManager::getInstance().startBruteForce();
            else {
                SpiLock lock(500); 
                if (lock.locked() && SD.exists("/last_capture.sub")) SubGhzManager::getInstance().playFlipperFile("/last_capture.sub"); 
                else SubGhzManager::getInstance().playFlipperFile("/test.sub"); 
            }
            break;
        default: break;
    }
}

void TaskUI(void* pvParameters) {
    auto& input = InputManager::getInstance();
    auto& display = DisplayManager::getInstance();
    auto& sys = SystemController::getInstance();
    auto& leds = LedManager::getInstance();

    input.init(); display.init(); leds.init();
    display.showSplashScreen(); vTaskDelay(pdMS_TO_TICKS(1500));

    StatusMessage statusMsg; statusMsg.state = SystemState::IDLE;
    CommandMessage cmdOut; TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t selectPressTime = 0; bool selectHeld = false;

    for (;;) {
        InputEvent evt = input.poll();
        if (evt != InputEvent::NONE) {
            display.handleInput(evt);
            if (evt == InputEvent::BTN_SELECT) { if (selectPressTime == 0) selectPressTime = millis(); } 
            else if (evt == InputEvent::BTN_BACK) { cmdOut.cmd = SystemCommand::CMD_STOP_ATTACK; sys.sendCommand(cmdOut); }
        }
        
        if (digitalRead(Config::PIN_BTN_SELECT) == LOW) { 
             if (selectPressTime > 0 && (millis() - selectPressTime > 1500) && !selectHeld) {
                 if (statusMsg.state == SystemState::IDLE) {
                     cmdOut.cmd = SystemCommand::CMD_SAVE_SETTINGS; sys.sendCommand(cmdOut);
                     display.drawPopup("Stealth Mode Toggle"); display.render(); vTaskDelay(500);
                 }
                 selectHeld = true;
             }
        } else {
            if (selectPressTime > 0 && !selectHeld) {
                if (statusMsg.state == SystemState::IDLE) {
                    int idx = display.getMenuIndex(); cmdOut.param1 = 0;
                    if (idx == 0) cmdOut.cmd = SystemCommand::CMD_START_SCAN_WIFI;
                    else if (idx == 1) { statusMsg.state = SystemState::SCAN_COMPLETE; display.updateStatus(statusMsg); display.setTargetList(sys.getScanResults()); } 
                    else if (idx == 2) cmdOut.cmd = SystemCommand::CMD_START_DEAUTH;
                    else if (idx == 3) cmdOut.cmd = SystemCommand::CMD_START_BEACON_SPAM;
                    else if (idx == 4) cmdOut.cmd = SystemCommand::CMD_START_EVIL_TWIN;
                    else if (idx == 5) { statusMsg.state = SystemState::MENU_SELECT_BLE; display.resetSubmenuIndex(); display.updateStatus(statusMsg); }
                    else if (idx == 6) { statusMsg.state = SystemState::MENU_SELECT_NRF; display.resetSubmenuIndex(); display.updateStatus(statusMsg); }
                    else if (idx == 7) cmdOut.cmd = SystemCommand::CMD_START_NRF_JAM;
                    else if (idx == 8) cmdOut.cmd = SystemCommand::CMD_START_NRF_ANALYZER;
                    else if (idx == 9) cmdOut.cmd = SystemCommand::CMD_START_NRF_SNIFF;
                    else if (idx == 10) cmdOut.cmd = SystemCommand::CMD_START_MOUSEJACK;
                    else if (idx == 11) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_SCAN;
                    else if (idx == 12) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_JAM;
                    else if (idx == 13) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_RX; 
                    else if (idx == 14) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_TX; 
                    else if (idx == 15) cmdOut.cmd = SystemCommand::CMD_START_ADMIN_MODE;
                    else if (idx == 16) cmdOut.cmd = SystemCommand::CMD_STOP_ATTACK;
                    
                    if (statusMsg.state == SystemState::IDLE) sys.sendCommand(cmdOut);
                } 
                else if (statusMsg.state == SystemState::SCAN_COMPLETE) {
                    if (strstr(statusMsg.logMsg, "Code Captured") != NULL) {
                        cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_TX; cmdOut.param1 = 0; sys.sendCommand(cmdOut);
                    } else {
                        cmdOut.cmd = SystemCommand::CMD_SELECT_TARGET; cmdOut.param1 = display.getTargetIndex(); sys.sendCommand(cmdOut);
                        cmdOut.cmd = SystemCommand::CMD_START_DEAUTH; cmdOut.param1 = 0; sys.sendCommand(cmdOut);
                    }
                }
            }
            selectPressTime = 0; selectHeld = false;
        }
        
        StatusMessage newMsg;
        if (sys.getStatus(newMsg)) {
            statusMsg = newMsg; display.updateStatus(statusMsg); leds.setStatus(statusMsg); 
            if (statusMsg.state == SystemState::SCAN_COMPLETE) display.setTargetList(sys.getScanResults());
        }
        leds.update(); display.render(); vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(33));
    }
}

void setup() {
    Serial.begin(115200);
    xTaskCreatePinnedToCore(TaskWorker, "Worker", 10000, NULL, 1, &g_TaskWorker, 0);
    xTaskCreatePinnedToCore(TaskUI, "UI", 5000, NULL, 1, &g_TaskUI, 1);
    vTaskDelete(NULL);
}

void loop() {}