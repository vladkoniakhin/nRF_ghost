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
    
    // WDT Init (5 seconds)
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);
    
    LedManager::getInstance().init();

    // Critical SD Check
    {
        SpiLock lock(2000);
        if (lock.locked()) {
            SdManager::getInstance().init();
            if (!SdManager::getInstance().isMounted()) {
                Serial.println("[SYS] CRITICAL: SD Card not found!");
                StatusMessage err; 
                err.state = SystemState::SD_ERROR;
                // Infinite loop with WDT reset to show error
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

    // Load Config & Scripts
    ConfigManager::getInstance().init();
    ScriptManager::getInstance().init();

    // Other subsystems
    SettingsManager::getInstance().init();
    UpdateManager::performUpdateIfAvailable();
    
    // Engines Init
    _wifiEngine.setup();
    BleManager::getInstance().setup();
    NrfManager::getInstance().setup();
    SubGhzManager::getInstance().setup();
    
    Serial.println("[SYS] System Ready. v5.6 Patched");
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

// --- JSON SERIAL HELPERS (NO HEAP ALLOC) ---

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
    // Используем StaticJsonDocument, чтобы не фрагментировать кучу
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

// --- MAIN LOOP ---

void SystemController::runWorkerLoop() {
    CommandMessage cmd; 
    StatusMessage statusOut; 
    memset(&statusOut, 0, sizeof(StatusMessage));
    
    uint32_t lastWsPush = 0;

    for (;;) {
        esp_task_wdt_reset();

        // 1. WebSocket Push Logic
        if (millis() - lastWsPush > 100) { 
            if (_currentState == SystemState::ADMIN_MODE) {
                WebPortalManager::getInstance().processDns(); 
                WebPortalManager::getInstance().broadcastStatus(statusOut.logMsg, ESP.getFreeHeap());
            }
            lastWsPush = millis();
        }

        // 2. Command Queue
        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) {
            processCommand(cmd);
        }

        // 3. Serial Handling (Heap Safe)
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') {
                g_serialBuffer[g_serialIndex] = '\0'; 
                
                // Обработка JSON без создания String
                if (g_serialBuffer[0] == '{') {
                    parseSerialJson(g_serialBuffer);
                } else {
                    // Simple text check via C-string functions
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
                    // Buffer overflow protection
                    g_serialIndex = 0; 
                    sendJsonError("Buffer Overflow");
                }
            }
        }

        // 4. Engine Loop
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

    // DISPATCHER
    switch (cmd.cmd) {
        case SystemCommand::CMD_START_SCAN_WIFI: 
            _activeEngine = &_wifiEngine; 
            _wifiEngine.startScan(); 
            break;

        case SystemCommand::CMD_START_DEAUTH: 
            if (_selectedTarget.bssid[0] != 0) { 
                _activeEngine = &_wifiEngine; 
                _wifiEngine.startDeauth(_selectedTarget); 
            } 
            break;

        case SystemCommand::CMD_START_EVIL_TWIN: 
            if (_selectedTarget.bssid[0] != 0) { 
                _activeEngine = &_wifiEngine; 
                _wifiEngine.startEvilTwin(_selectedTarget); 
            } 
            break;

        case SystemCommand::CMD_START_BEACON_SPAM: 
            _activeEngine = &_wifiEngine; 
            _wifiEngine.startBeaconSpam(); 
            break;
            
        case SystemCommand::CMD_START_BLE_SPOOF: 
            _activeEngine = &BleManager::getInstance(); 
            BleManager::getInstance().startSpoof((BleSpoofType)cmd.param1); 
            break;
            
        case SystemCommand::CMD_START_NRF_JAM: 
            _activeEngine = &NrfManager::getInstance(); 
            NrfManager::getInstance().startJamming((uint8_t)cmd.param1); 
            break;

        case SystemCommand::CMD_START_NRF_ANALYZER: 
            _activeEngine = &NrfManager::getInstance(); 
            NrfManager::getInstance().startAnalyzer(); 
            break;

        case SystemCommand::CMD_START_MOUSEJACK: 
            _activeEngine = &NrfManager::getInstance(); 
            NrfManager::getInstance().startMouseJack(0); 
            break;

        case SystemCommand::CMD_START_NRF_SNIFF: 
            _activeEngine = &NrfManager::getInstance(); 
            NrfManager::getInstance().startSniffing(); 
            break;
            
        case SystemCommand::CMD_START_SUBGHZ_SCAN: 
            _activeEngine = &SubGhzManager::getInstance(); 
            SubGhzManager::getInstance().startAnalyzer(); 
            break;

        case SystemCommand::CMD_START_SUBGHZ_JAM: 
            _activeEngine = &SubGhzManager::getInstance(); 
            SubGhzManager::getInstance().startJammer(); 
            break;

        case SystemCommand::CMD_START_SUBGHZ_RX: 
            _activeEngine = &SubGhzManager::getInstance(); 
            SubGhzManager::getInstance().startCapture(); 
            break;

        case SystemCommand::CMD_START_SUBGHZ_TX: 
            _activeEngine = &SubGhzManager::getInstance(); 
            if (cmd.param1 == 1) {
                SubGhzManager::getInstance().startBruteForce();
            } else {
                SubGhzManager::getInstance().playFlipperFile("/test.sub"); 
            }
            break;
            
        default: break;
    }
}