#include "System.h"
#include "DisplayManager.h"
#include "SdManager.h"
#include "UpdateManager.h"
#include "WiFiManager.h"
#include "BleManager.h"
#include "NrfManager.h"
#include "SubGhzManager.h"
#include "WebPortalManager.h" 
#include <esp_task_wdt.h>

SemaphoreHandle_t g_spiMutex = nullptr;

// Статический буфер для Serial (защита от фрагментации кучи и Deadlock)
static char g_serialBuffer[256]; 
static uint16_t g_serialIndex = 0;

// RAII Wrapper
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
    
    // 1. Init LED first to show errors
    LedManager::getInstance().init();

    // 2. Critical Hardware Check (UX-01 Fix)
    {
        SpiLock lock(2000);
        if (lock.locked()) {
            SdManager::getInstance().init();
            if (!SdManager::getInstance().isMounted()) {
                Serial.println("[SYS] CRITICAL: SD Card not found!");
                
                // FATAL ERROR LOOP
                // We trap the system here so the user clearly sees the issue via LED
                StatusMessage err; 
                err.state = SystemState::SD_ERROR;
                
                while(true) {
                    LedManager::getInstance().setStatus(err);
                    LedManager::getInstance().update();
                    delay(10); // Allow WDT reset in background if configured
                }
            }
        } else {
            Serial.println("[SYS] Critical: SPI Mutex Deadlock on Boot");
        }
    }

    SettingsManager::getInstance().init();
    UpdateManager::performUpdateIfAvailable();
    
    _wifiEngine.setup();
    BleManager::getInstance().setup();
    NrfManager::getInstance().setup();
    SubGhzManager::getInstance().setup();
}

void SystemController::stopCurrentTask() {
    if (_activeEngine) { 
        _activeEngine->stop(); 
        _activeEngine = nullptr; 
    }
    
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

// --- SECURE PROTOCOL PARSER ---

void SystemController::sendJsonSuccess(const char* msg) {
    Serial.printf("{\"status\":\"ok\",\"msg\":\"%s\"}\n", msg);
}

void SystemController::sendJsonError(const char* err) {
    Serial.printf("{\"status\":\"error\",\"msg\":\"%s\"}\n", err);
}

void SystemController::sendJsonFileList(const char* path) {
    SpiLock lock(1000);
    if (lock.locked()) {
        if (!SD.exists(path)) {
             sendJsonError("Path not found");
             return;
        }
        File root = SD.open(path);
        if (!root || !root.isDirectory()) {
            sendJsonError("Not a directory");
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

// Optimized non-allocating parser
void SystemController::parseSerialJson(String& input) {
    input.trim();
    // Basic validation
    if (input.length() < 5 || input.charAt(0) != '{') {
        return; // Silent ignore or error
    }

    char buffer[128];
    if (input.length() >= sizeof(buffer) - 1) {
        sendJsonError("Cmd too long");
        return;
    }

    strncpy(buffer, input.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0'; 

    for(int i=0; buffer[i]; i++) buffer[i] = toupper((unsigned char)buffer[i]);

    char* cmdPtr = strstr(buffer, "\"CMD\"");
    if (!cmdPtr) return;

    cmdPtr = strchr(cmdPtr, ':');
    if (!cmdPtr) return;
    cmdPtr++; 
    
    while(*cmdPtr == ' ' || *cmdPtr == '"') cmdPtr++;
    
    char* endPtr = strpbrk(cmdPtr, "\",}");
    if (endPtr) *endPtr = '\0';

    if (strcmp(cmdPtr, "SCAN") == 0) {
        processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
        sendJsonSuccess("Scan started");
    }
    else if (strcmp(cmdPtr, "STOP") == 0) {
        processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
        sendJsonSuccess("Stopped");
    }
    else if (strcmp(cmdPtr, "LIST") == 0) {
        sendJsonFileList("/");
    }
    else if (strcmp(cmdPtr, "JAM") == 0) {
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
    
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);

    for (;;) {
        esp_task_wdt_reset();

        // 1. Queue Processing
        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) {
            processCommand(cmd);
        }

        // 2. Serial Processing (Non-blocking Fix STAB-01)
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') {
                g_serialBuffer[g_serialIndex] = '\0'; // Null-terminate
                String line = String(g_serialBuffer); // Safe wrapper for parser
                
                if (line.startsWith("{")) {
                    parseSerialJson(line);
                } else {
                    // Legacy Text Commands
                    line.trim();
                    if (line == "SCAN") processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
                    else if (line == "STOP") processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
                    else if (line == "STATUS") Serial.println("OK"); 
                }
                
                // Reset buffer
                g_serialIndex = 0;
            } 
            else {
                if (g_serialIndex < sizeof(g_serialBuffer) - 1) {
                    g_serialBuffer[g_serialIndex++] = c;
                } else {
                    // Buffer overflow protection: Reset and ignore
                    g_serialIndex = 0; 
                    sendJsonError("Serial Buffer Overflow");
                }
            }
        }

        // 3. Engine Loop
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
        WebPortalManager::getInstance().start("nRF_Admin");
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
        case SystemCommand::CMD_START_SUBGHZ_TX: _activeEngine = &SubGhzManager::getInstance(); SubGhzManager::getInstance().startReplay(); break;
        default: break;
    }
}