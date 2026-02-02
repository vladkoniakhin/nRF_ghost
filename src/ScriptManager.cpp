#include "ScriptManager.h"
#include "System.h"
#include "SubGhzManager.h"

ScriptManager& ScriptManager::getInstance() {
    static ScriptManager instance;
    return instance;
}

ScriptManager::ScriptManager() : _taskHandle(nullptr), _running(false), _signalReceived(false) {}

void ScriptManager::init() {
    // Basic init if needed
}

void ScriptManager::stop() {
    _running = false;
    if (_taskHandle) {
        vTaskDelete(_taskHandle);
        _taskHandle = nullptr;
    }
}

void ScriptManager::notifySignal(uint32_t signalHash) {
    if (_running) {
        _lastHash = signalHash;
        _signalReceived = true;
    }
}

void ScriptManager::runScript(const char* path) {
    if (!SD.exists(path)) {
        Serial.println("[SCRIPT] File not found");
        return;
    }
    stop();
    strncpy(_currentScriptPath, path, 63);
    _running = true;
    
    // Запуск задачи с низким приоритетом, чтобы не мешать радио
    xTaskCreatePinnedToCore(scriptTask, "ScriptEng", 4096, this, 1, &_taskHandle, 1);
}

void ScriptManager::scriptTask(void* param) {
    ScriptManager* mgr = (ScriptManager*)param;
    
    Serial.printf("[SCRIPT] Running %s\n", mgr->_currentScriptPath);
    
    // Считываем файл один раз
    // В идеале - кэшировать в RAM, но для экономии памяти читаем построчно
    File file = SD.open(mgr->_currentScriptPath);
    if (!file) {
        mgr->_running = false;
        vTaskDelete(NULL);
    }

    mgr->parseAndExecute(file);
    
    file.close();
    mgr->_running = false;
    Serial.println("[SCRIPT] Finished");
    vTaskDelete(NULL);
}

ScriptLine ScriptManager::parseLine(String line) {
    ScriptLine sl;
    sl.cmd = ScriptCmd::NONE;
    line.trim();
    if (line.length() == 0 || line.startsWith("//")) return sl;

    int spaceIdx = line.indexOf(' ');
    String cmdStr = (spaceIdx == -1) ? line : line.substring(0, spaceIdx);
    String argStr = (spaceIdx == -1) ? "" : line.substring(spaceIdx + 1);
    
    cmdStr.toUpperCase();
    argStr.trim();
    if (argStr.startsWith("\"") && argStr.endsWith("\"")) {
        argStr = argStr.substring(1, argStr.length()-1);
    }

    sl.arg = argStr;

    if (cmdStr == "WAIT_RX") {
        sl.cmd = ScriptCmd::WAIT_RX;
        sl.val = (uint32_t)(argStr.toFloat() * 1000000); // Freq logic placeholder
    } else if (cmdStr == "IF_SIGNAL") {
        sl.cmd = ScriptCmd::IF_SIGNAL;
        sl.val = strtoul(argStr.c_str(), NULL, 16);
    } else if (cmdStr == "TX_RAW") {
        sl.cmd = ScriptCmd::TX_RAW;
    } else if (cmdStr == "LOG") {
        sl.cmd = ScriptCmd::LOG_MSG;
    } else if (cmdStr == "DELAY") {
        sl.cmd = ScriptCmd::DELAY_MS;
        sl.val = argStr.toInt();
    }

    return sl;
}

void ScriptManager::parseAndExecute(File& file) {
    while (file.available() && _running) {
        String line = file.readStringUntil('\n');
        ScriptLine sl = parseLine(line);

        switch (sl.cmd) {
            case ScriptCmd::WAIT_RX:
                Serial.println("[SCRIPT] Waiting for Signal...");
                // Переключаем SubGhz в режим RX
                SubGhzManager::getInstance().startCapture(); 
                _signalReceived = false;
                // Ждем сигнала или стопа
                while (!_signalReceived && _running) {
                    vTaskDelay(10);
                }
                break;

            case ScriptCmd::IF_SIGNAL:
                if (_signalReceived && _lastHash == sl.val) {
                    Serial.println("[SCRIPT] Hash Match!");
                } else {
                    // Пропускаем следующую строку (простейший IF)
                    // TODO: Реализовать блоки {} для v5.1
                    if (file.available()) file.readStringUntil('\n'); 
                }
                break;

            case ScriptCmd::TX_RAW:
                Serial.printf("[SCRIPT] TX: %s\n", sl.arg.c_str());
                SubGhzManager::getInstance().playFlipperFile(sl.arg.c_str());
                // Ждем окончания передачи (проверяем флаг в SubGhzManager)
                while(SubGhzManager::getInstance().isReplaying()) vTaskDelay(10);
                break;

            case ScriptCmd::LOG_MSG:
                Serial.printf("[SCRIPT] LOG: %s\n", sl.arg.c_str());
                // Можно отправить в WebSocket
                break;

            case ScriptCmd::DELAY_MS:
                vTaskDelay(sl.val);
                break;
                
            default: break;
        }
    }
}