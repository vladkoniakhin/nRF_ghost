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
    if (_activeEngine) { _activeEngine->stop(); _activeEngine = nullptr; }
    if (_currentState == SystemState::ADMIN_MODE) WebPortalManager::getInstance().stop();
    _currentState = SystemState::IDLE;
}

bool SystemController::sendCommand(CommandMessage cmd) { return xQueueSend(_commandQueue, &cmd, 0) == pdTRUE; }
bool SystemController::getStatus(StatusMessage& msg) { return xQueueReceive(_statusQueue, &msg, 0) == pdTRUE; }

void SystemController::runWorkerLoop() {
    CommandMessage cmd; 
    StatusMessage statusOut; 
    memset(&statusOut, 0, sizeof(StatusMessage));
    
    for (;;) {
        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) processCommand(cmd);

        if (Serial.available()) {
            String line = Serial.readStringUntil('\n'); line.trim();
            if (line == "SCAN") processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
            else if (line == "STOP") processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
            else if (line.startsWith("JAM")) {
                int ch = line.substring(4).toInt();
                processCommand({SystemCommand::CMD_START_NRF_JAM, ch});
            }
            Serial.println("OK");
        }

        bool running = false;
        
        if (_currentState == SystemState::ADMIN_MODE) {
            statusOut.state = SystemState::ADMIN_MODE; 
            statusOut.logMsg[0] = '\0';
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
    // 1. Обработка выбора цели (это не атака, можно делать всегда)
    if (cmd.cmd == SystemCommand::CMD_SELECT_TARGET) {
        auto results = _wifiEngine.getScanResults();
        if (cmd.param1 >= 0 && cmd.param1 < (int)results.size()) _selectedTarget = results[cmd.param1];
        return;
    }

    // 2. Команда остановки всегда имеет приоритет
    if (cmd.cmd == SystemCommand::CMD_STOP_ATTACK) {
        stopCurrentTask();
        return;
    }

    // 3. АДМИН-ПАНЕЛЬ (Web)
    // Разрешаем вход в админку, только если ничего не запущено
    if (cmd.cmd == SystemCommand::CMD_START_ADMIN_MODE) {
        if (_activeEngine != nullptr) {
            // ОШИБКА: Нельзя войти в админку во время атаки
            return; 
        }
        _currentState = SystemState::ADMIN_MODE;
        WebPortalManager::getInstance().start("nRF_Admin");
        return;
    }

    // 4. ЗАЩИТА ОТ BROWNOUT (Самое важное!)
    // Если уже работает какой-то Engine (WiFi, NRF, SubGhz), 
    // мы ЗАПРЕЩАЕМ запуск нового. Пользователь обязан нажать STOP.
    if (_activeEngine != nullptr) {
        // Тут можно добавить отправку сообщения на экран "BUSY! PRESS STOP"
        // Но пока просто игнорируем команду, чтобы не спалить питание
        Serial.println("[Error] System Busy. Stop current attack first!");
        return; 
    }

    // 5. Запуск новых задач (только если _activeEngine == nullptr)
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