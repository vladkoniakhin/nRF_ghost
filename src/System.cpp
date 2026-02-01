#include "System.h"
#include "DisplayManager.h"
#include "SdManager.h"
#include "UpdateManager.h"
#include "WiFiManager.h"
#include "BleManager.h"
#include "NrfManager.h"
#include "SubGhzManager.h"
#include "WebPortalManager.h" 
#include <esp_task_wdt.h> // Для сброса Watchdog

SemaphoreHandle_t g_spiMutex = nullptr;

// RAII Wrapper для мьютекса (гарантирует разблокировку при выходе из scope)
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
    
    // Инициализация SPI и SD с защитой
    {
        SpiLock lock(2000);
        if (lock.locked()) {
            SdManager::getInstance().init();
        } else {
            Serial.println("[SYS] Critical: Failed to init SD (Mutex timeout)");
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

// --- БЕЗОПАСНЫЙ ПРОТОКОЛ (Zero-Copy Parser) ---

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
                // Защита от зависания при длинном листинге
                esp_task_wdt_reset(); 
                if (!first) Serial.print(",");
                // Используем имя файла осторожно
                const char* name = file.name();
                // Пропускаем системные файлы
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

// Парсер без использования String (No Heap Fragmentation)
void SystemController::parseSerialJson(String& input) {
    // 1. Быстрая проверка валидности
    input.trim();
    if (input.length() < 5 || input.charAt(0) != '{' || input.charAt(input.length()-1) != '}') {
        sendJsonError("Invalid JSON");
        return;
    }

    // 2. Копируем в статический буфер для безопасного парсинга
    char buffer[128];
    // Защита от переполнения буфера
    if (input.length() >= sizeof(buffer)) {
        sendJsonError("Cmd too long");
        return;
    }
    strcpy(buffer, input.c_str());

    // 3. "Грязный" парсинг без тяжелых библиотек (ищем "CMD":"VALUE")
    // Приводим к верхнему регистру вручную, чтобы не аллоцировать String
    for(int i=0; buffer[i]; i++) buffer[i] = toupper((unsigned char)buffer[i]);

    // Ищем ключ
    char* cmdPtr = strstr(buffer, "\"CMD\"");
    if (!cmdPtr) { sendJsonError("No CMD found"); return; }

    // Ищем значение (пропускаем "CMD" и двоеточие)
    cmdPtr = strchr(cmdPtr, ':');
    if (!cmdPtr) return;
    cmdPtr++; // Пропускаем :
    
    // Очистка от кавычек и пробелов
    while(*cmdPtr == ' ' || *cmdPtr == '"') cmdPtr++;
    
    // Находим конец значения
    char* endPtr = strpbrk(cmdPtr, "\",}");
    if (endPtr) *endPtr = '\0';

    // 4. Диспетчеризация
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
    
    // Включаем WDT для этой задачи (3 секунды тайм-аут)
    esp_task_wdt_init(3, true);
    esp_task_wdt_add(NULL);

    for (;;) {
        // Сброс собаки в главном цикле
        esp_task_wdt_reset();

        // 1. Очередь команд
        if (xQueueReceive(_commandQueue, &cmd, 0) == pdTRUE) {
            processCommand(cmd);
        }

        // 2. Serial (с защитой от переполнения буфера Serial)
        if (Serial.available()) {
            // Читаем с таймаутом, чтобы не висеть вечно
            String line = Serial.readStringUntil('\n'); 
            if (line.length() > 0) {
                if (line.startsWith("{")) {
                    parseSerialJson(line);
                } else {
                    // Legacy Text Mode
                    line.trim();
                    if (line == "SCAN") processCommand({SystemCommand::CMD_START_SCAN_WIFI, 0});
                    else if (line == "STOP") processCommand({SystemCommand::CMD_STOP_ATTACK, 0});
                    else if (line.startsWith("JAM")) processCommand({SystemCommand::CMD_START_NRF_JAM, 40});
                    Serial.println("OK");
                }
            }
        }

        // 3. Логика движка
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

    // Mutex Logic
    if (_activeEngine != nullptr) {
        Serial.println("[Err] System BUSY. Please press STOP first.");
        return; 
    }

    // Запуск движков
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