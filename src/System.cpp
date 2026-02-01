#include "System.h"
#include "DisplayManager.h"
#include "SdManager.h"
#include "UpdateManager.h"
#include "WiFiManager.h"
#include "BleManager.h"
#include "NrfManager.h"
#include "SubGhzManager.h"
#include "WebPortalManager.h" 

SemaphoreHandle_t g_spiMutex = nullptr;

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
    SettingsManager::getInstance().init();
    SdManager::getInstance().init();
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
    
    // При выходе из админки - останавливаем веб-сервер
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

// --- РЕАЛИЗАЦИЯ JSON ПРОТОКОЛА ---

void SystemController::sendJsonSuccess(const char* msg) {
    Serial.printf("{\"status\":\"ok\",\"msg\":\"%s\"}\n", msg);
}

void SystemController::sendJsonError(const char* err) {
    Serial.printf("{\"status\":\"error\",\"msg\":\"%s\"}\n", err);
}

void SystemController::sendJsonFileList(const char* path) {
    if (xSemaphoreTake(g_spiMutex, 1000)) {
        File root = SD.open(path);
        if (!root || !root.isDirectory()) {
            sendJsonError("Bad path");
        } else {
            Serial.print("{\"files\":[");
            File file = root.openNextFile();
            bool first = true;
            while (file) {
                if (!first) Serial.print(",");
                Serial.printf("{\"n\":\"%s\",\"s\":%d}", file.name(), file.size());
                first = false;
                file = root.openNextFile();
            }
            Serial.println("]}");
        }
        xSemaphoreGive(g_spiMutex);
    } else {
        sendJsonError("SPI Busy");
    }
}

void SystemController::parseSerialJson(String& line) {
    // Простой парсер без библиотек (для экономии памяти)
    line.trim();
    if (!line.startsWith("{") || !line.endsWith("}")) return;

    String cmdUpper = line; 
    cmdUpper.toUpperCase();

    if (cmdUpper.indexOf("\"CMD\":\"SCAN\"") > 0) {
        processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
        sendJsonSuccess("Scan started");
    }
    else if (cmdUpper.indexOf("\"CMD\":\"STOP\"") > 0) {
        processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
        sendJsonSuccess("Stopped");
    }
    else if (cmdUpper.indexOf("\"CMD\":\"LIST\"") > 0) {
        sendJsonFileList("/");
    }
    else if (cmdUpper.indexOf("\"CMD\":\"JAM\"") > 0) {
        // Пример запуска глушилки NRF на канале 40
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
    
    for (;;) {
        // 1. Команды из очереди (от кнопок UI)
        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) {
            processCommand(cmd);
        }

        // 2. Команды из Serial (PC Client / JSON)
        if (Serial.available()) {
            String line = Serial.readStringUntil('\n'); 
            if (line.startsWith("{")) {
                parseSerialJson(line);
            } else {
                // Старый текстовый режим
                line.trim();
                if (line == "SCAN") processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
                else if (line == "STOP") processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
                else if (line.startsWith("JAM")) processCommand({SystemCommand::CMD_START_NRF_JAM, 40});
                Serial.println("OK");
            }
        }

        // 3. Выполнение активной задачи
        bool running = false;
        
        if (_currentState == SystemState::ADMIN_MODE) {
            statusOut.state = SystemState::ADMIN_MODE; 
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Web Admin Mode");
            running = true;
        } 
        else if (_activeEngine) {
            running = _activeEngine->loop(statusOut);
            
            if (!running) {
                // Если задача завершилась сама (например, сканирование завершено)
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
    // 1. Разрешенные команды в любое время (неблокирующие)
    if (cmd.cmd == SystemCommand::CMD_SELECT_TARGET) {
        auto results = _wifiEngine.getScanResults();
        if (cmd.param1 >= 0 && cmd.param1 < (int)results.size()) _selectedTarget = results[cmd.param1];
        return;
    }

    // 2. Команда STOP всегда имеет приоритет
    if (cmd.cmd == SystemCommand::CMD_STOP_ATTACK) {
        stopCurrentTask();
        return;
    }

    // 3. Вход в Admin Mode (Web)
    if (cmd.cmd == SystemCommand::CMD_START_ADMIN_MODE) {
        if (_activeEngine != nullptr) {
            // Нельзя зайти в админку, если идет атака
            Serial.println("[Err] Stop current task first!");
            return; 
        }
        _currentState = SystemState::ADMIN_MODE;
        WebPortalManager::getInstance().start("nRF_Admin");
        return;
    }

    // 4. ЗАЩИТА ОТ КОЛЛИЗИЙ (MUTEX LOGIC)
    // Если что-то уже запущено - запрещаем запуск новой задачи
    if (_activeEngine != nullptr) {
        Serial.println("[Err] System BUSY. Please press STOP first.");
        return; 
    }

    // 5. Запуск движков
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
            SubGhzManager::getInstance().startReplay(); 
            break;
            
        default: break;
    }
}