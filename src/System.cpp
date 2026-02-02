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
#include <esp_task_wdt.h>
#include <ArduinoJson.h>

SemaphoreHandle_t g_spiMutex = nullptr;
static char g_serialBuffer[256]; 
static uint16_t g_serialIndex = 0;

class SpiLock {
public:
    SpiLock(uint32_t timeoutMs = 1000) {
        if (g_spiMutex) {
            _acquired = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(timeoutMs));
        }
    }
    ~SpiLock() {
        if (_acquired && g_spiMutex) {
            xSemaphoreGive(g_spiMutex);
        }
    }
    bool locked() const { return _acquired; }
private:
    bool _acquired = false;
};

SystemController& SystemController::getInstance() {
    static SystemController instance;
    return instance;
}

SystemController::SystemController() : _currentState(SystemState::IDLE), _activeEngine(nullptr) {
    _commandQueue = xQueueCreate(10, sizeof(CommandMessage));
    _statusQueue = xQueueCreate(1, sizeof(StatusMessage));
}

void SystemController::init() {
    g_spiMutex = xSemaphoreCreateMutex();
    
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);
    
    LedManager::getInstance().init();

    {
        SpiLock lock(2000);
        if (lock.locked()) {
            SdManager::getInstance().init();
            if (!SdManager::getInstance().isMounted()) {
                Serial.println("[SYS] CRITICAL: SD Card not found!");
                StatusMessage err; 
                err.state = SystemState::SD_ERROR;
                while(true) {
                    esp_task_wdt_reset();
                    LedManager::getInstance().setStatus(err);
                    LedManager::getInstance().update();
                    delay(10); 
                }
            }
        } else {
            Serial.println("[SYS] Critical: SPI Mutex Deadlock on Boot");
        }
    }

    ConfigManager::getInstance().init();
    ScriptManager::getInstance().init();
    SettingsManager::getInstance().init();
    UpdateManager::performUpdateIfAvailable();
    
    _wifiEngine.setup();
    BleManager::getInstance().setup();
    NrfManager::getInstance().setup();
    SubGhzManager::getInstance().setup();
    
    Serial.println("[SYS] System Ready. v6.0 Final");
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
    Serial.println("[SYS] Stopped. Idle.");
}

bool SystemController::sendCommand(CommandMessage cmd) { 
    return xQueueSend(_commandQueue, &cmd, 0) == pdTRUE; 
}

bool SystemController::getStatus(StatusMessage& msg) { 
    return xQueueReceive(_statusQueue, &msg, 0) == pdTRUE; 
}

void SystemController::sendJsonSuccess(const char* msg) {
    Serial.printf("{\"status\":\"ok\",\"msg\":\"%s\"}\n", msg);
}

void SystemController::sendJsonError(const char* err) {
    Serial.printf("{\"status\":\"error\",\"msg\":\"%s\"}\n", err);
}

void SystemController::sendJsonFileList(const char* path) {
    SpiLock lock(1000);
    if (lock.locked()) {
        File root = SD.open(path);
        if (!root || !root.isDirectory()) {
            sendJsonError("Bad path");
        } else {
            Serial.print("{\"files\":[");
            File file = root.openNextFile();
            bool first = true;
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
    } else {
        sendJsonError("SPI Busy");
    }
}

void SystemController::parseSerialJson(char* input) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, input);

    if (error) {
        sendJsonError("Invalid JSON");
        return;
    }

    const char* cmdStr = doc["CMD"];
    if (!cmdStr) return;

    if (strcmp(cmdStr, "SCAN") == 0) {
        processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
        sendJsonSuccess("Scan started");
    }
    else if (strcmp(cmdStr, "STOP") == 0) {
        processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
        sendJsonSuccess("Stopped");
    }
    else if (strcmp(cmdStr, "LIST") == 0) {
        sendJsonFileList("/");
    }
    else if (strcmp(cmdStr, "JAM") == 0) {
        processCommand({SystemCommand::CMD_START_NRF_JAM, 40});
        sendJsonSuccess("Jamming started");
    }
    else {
        sendJsonError("Unknown command");
    }
}

void SystemController::runWorkerLoop() {
    CommandMessage cmd; 
    StatusMessage statusOut; 
    memset(&statusOut, 0, sizeof(StatusMessage));
    
    uint32_t lastWsPush = 0;

    for (;;) {
        esp_task_wdt_reset();

        if (millis() - lastWsPush > 100) { 
            if (_currentState == SystemState::ADMIN_MODE) {
                WebPortalManager::getInstance().processDns(); 
                WebPortalManager::getInstance().broadcastStatus(statusOut.logMsg, ESP.getFreeHeap());
            }
            lastWsPush = millis();
        }

        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) {
            processCommand(cmd);
        }

        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') {
                g_serialBuffer[g_serialIndex] = '\0'; 
                if (g_serialBuffer[0] == '{') {
                    parseSerialJson(g_serialBuffer);
                } else {
                    if (strncmp(g_serialBuffer, "SCAN", 4) == 0) processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
                    else if (strncmp(g_serialBuffer, "STOP", 4) == 0) processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
                    else if (strncmp(g_serialBuffer, "STATUS", 6) == 0) Serial.println("OK");
                }
                g_serialIndex = 0;
            } 
            else {
                if (g_serialIndex < sizeof(g_serialBuffer) - 1) {
                    g_serialBuffer[g_serialIndex++] = c;
                } else {
                    g_serialIndex = 0; 
                    sendJsonError("Buffer Overflow");
                }
            }
        }

        bool running = false;
        
        if (_currentState == SystemState::ADMIN_MODE) {
            statusOut.state = SystemState::ADMIN_MODE; 
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Web Admin Mode");
            running = true;
        } 
        else if (_activeEngine) {
            running = _activeEngine->loop(statusOut);
            
            if (!running) {
                if (_activeEngine == &_wifiEngine && statusOut.state == SystemState::SCAN_COMPLETE) {
                       statusOut.state = SystemState::SCAN_COMPLETE; 
                       _currentState = SystemState::SCAN_COMPLETE;
                } else {
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

void SystemController::processCommand(CommandMessage cmd) {
    if (cmd.cmd == SystemCommand::CMD_SELECT_TARGET) {
        auto results = _wifiEngine.getScanResults();
        if (cmd.param1 >= 0 && cmd.param1 < (int)results.size()) _selectedTarget = results[cmd.param1];
        return;
    }

    if (cmd.cmd == SystemCommand::CMD_STOP_ATTACK) {
        stopCurrentTask();
        return;
    }

    if (cmd.cmd == SystemCommand::CMD_START_ADMIN_MODE) {
        if (_activeEngine != nullptr) {
            Serial.println("[Err] Stop current task first!");
            return; 
        }
        _currentState = SystemState::ADMIN_MODE;
        WebPortalManager::getInstance().start(""); 
        return;
    }

    if (_activeEngine != nullptr) {
        Serial.println("[Err] System BUSY.");
        return; 
    }

    switch (cmd.cmd) {
        case SystemCommand::CMD_START_SCAN_WIFI: _activeEngine = &_wifiEngine; _wifiEngine.startScan(); break;
        case SystemCommand::CMD_START_DEAUTH: if (_selectedTarget.bssid[0] != 0) { _activeEngine = &_wifiEngine; _wifiEngine.startDeauth(_selectedTarget); } break;
        case SystemCommand::CMD_START_EVIL_TWIN: if (_selectedTarget.bssid[0] != 0) { _activeEngine = &_wifiEngine; _wifiEngine.startEvilTwin(_selectedTarget); } break;
        case SystemCommand::CMD_START_BEACON_SPAM: _activeEngine = &_wifiEngine; _wifiEngine.startBeaconSpam(); break;
        case SystemCommand::CMD_START_BLE_SPOOF: _activeEngine = &BleManager::getInstance(); BleManager::getInstance().startSpoof((BleSpoofType)cmd.param1); break;
        case SystemCommand::CMD_START_NRF_JAM: _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startJamming((uint8_t)cmd.param1); break;
        case SystemCommand::CMD_START_NRF_ANALYZER: _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startAnalyzer(); break;
        case SystemCommand::CMD_START_MOUSEJACK: _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startMouseJack(0); break;
        case SystemCommand::CMD_START_NRF_SNIFF: _activeEngine = &NrfManager::getInstance(); NrfManager::getInstance().startSniffing(); break;
        case SystemCommand::CMD_START_SUBGHZ_SCAN: _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startAnalyzer(); break;
        case SystemCommand::CMD_START_SUBGHZ_JAM: _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startJammer(); break;
        case SystemCommand::CMD_START_SUBGHZ_RX: _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startCapture(); break;
        case SystemCommand::CMD_START_SUBGHZ_TX: 
            _activeEngine = &SubGhzManager::getInstance(); 
            if (cmd.param1 == 1) SubGhzManager::getInstance().startBruteForce();
            else SubGhzManager::getInstance().playFlipperFile("/test.sub"); 
            break;
        default: break;
    }
}

// FIX v6.0: PURE 4-BUTTON LOGIC (CLEANED)
void TaskUI(void* pvParameters) {
    auto& input = InputManager::getInstance();
    auto& display = DisplayManager::getInstance();
    auto& sys = SystemController::getInstance();
    auto& leds = LedManager::getInstance();

    input.init(); 
    display.init(); 
    leds.init();
    
    display.showSplashScreen(); 
    vTaskDelay(pdMS_TO_TICKS(1500));

    StatusMessage statusMsg; 
    statusMsg.state = SystemState::IDLE;
    CommandMessage cmdOut;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        InputEvent evt = input.poll();
        
        if (evt != InputEvent::NONE) {
            display.handleInput(evt);
            
            // CLEAN: Только SELECT (без BTN_RIGHT)
            if (evt == InputEvent::BTN_SELECT) {
                if (statusMsg.state == SystemState::IDLE) {
                    int idx = display.getMenuIndex(); 
                    cmdOut.param1 = 0;
                    
                    if (idx == 0) cmdOut.cmd = SystemCommand::CMD_START_SCAN_WIFI;
                    else if (idx == 1) cmdOut.cmd = SystemCommand::CMD_START_DEAUTH;
                    else if (idx == 2) cmdOut.cmd = SystemCommand::CMD_START_BEACON_SPAM;
                    else if (idx == 3) cmdOut.cmd = SystemCommand::CMD_START_EVIL_TWIN;
                    else if (idx == 4) { statusMsg.state = SystemState::MENU_SELECT_BLE; display.resetSubmenuIndex(); display.updateStatus(statusMsg); }
                    else if (idx == 5) { statusMsg.state = SystemState::MENU_SELECT_NRF; display.resetSubmenuIndex(); display.updateStatus(statusMsg); }
                    else if (idx == 6) cmdOut.cmd = SystemCommand::CMD_START_NRF_JAM;
                    else if (idx == 7) cmdOut.cmd = SystemCommand::CMD_START_NRF_ANALYZER;
                    else if (idx == 8) cmdOut.cmd = SystemCommand::CMD_START_NRF_SNIFF;
                    else if (idx == 9) cmdOut.cmd = SystemCommand::CMD_START_MOUSEJACK;
                    else if (idx == 10) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_SCAN;
                    else if (idx == 11) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_JAM;
                    else if (idx == 12) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_RX; 
                    else if (idx == 13) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_TX; 
                    else if (idx == 14) cmdOut.cmd = SystemCommand::CMD_START_ADMIN_MODE;
                    else if (idx == 15) cmdOut.cmd = SystemCommand::CMD_STOP_ATTACK;
                    
                    if (statusMsg.state == SystemState::IDLE) sys.sendCommand(cmdOut);
                } 
                else if (statusMsg.state == SystemState::SCAN_COMPLETE) {
                    cmdOut.cmd = SystemCommand::CMD_SELECT_TARGET; cmdOut.param1 = display.getTargetIndex(); sys.sendCommand(cmdOut);
                    cmdOut.cmd = SystemCommand::CMD_START_DEAUTH; cmdOut.param1 = 0; sys.sendCommand(cmdOut);
                }
            } else if (evt == InputEvent::BTN_BACK) {
                cmdOut.cmd = SystemCommand::CMD_STOP_ATTACK; sys.sendCommand(cmdOut);
            }
        }
        
        StatusMessage newMsg;
        if (sys.getStatus(newMsg)) {
            statusMsg = newMsg; 
            display.updateStatus(statusMsg); 
            leds.setStatus(statusMsg); 
            if (statusMsg.state == SystemState::SCAN_COMPLETE) display.setTargetList(sys.getScanResults());
        }
        
        leds.update(); 
        display.render();
        
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(33));
    }
}

void setup() {
    Serial.begin(115200);
    xTaskCreatePinnedToCore(TaskWorker, "Worker", 10000, NULL, 1, &g_TaskWorker, 0);
    xTaskCreatePinnedToCore(TaskUI, "UI", 5000, NULL, 1, &g_TaskUI, 1);
    vTaskDelete(NULL);
}

void loop() {}