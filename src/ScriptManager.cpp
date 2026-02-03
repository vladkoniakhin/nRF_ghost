#include "ScriptManager.h"
#include "System.h"
#include "SubGhzManager.h"

ScriptManager& ScriptManager::getInstance() {
    static ScriptManager instance;
    return instance;
}

ScriptManager::ScriptManager() : _taskHandle(nullptr), _running(false), _signalReceived(false) {}

void ScriptManager::init() {}

void ScriptManager::stop() {
    _running = false;
    if (_taskHandle) {
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
    if (!SD.exists(path)) return;
    stop();
    strncpy(_currentScriptPath, path, 63);
    _running = true;
    xTaskCreatePinnedToCore(scriptTask, "ScriptEng", 4096, this, 1, &_taskHandle, 1);
}

// Helper: Безопасное чтение строки в буфер
size_t readLine(File& f, char* buf, size_t maxLen) {
    size_t count = 0;
    while (f.available() && count < maxLen - 1) {
        char c = f.read();
        if (c == '\n') break;
        if (c != '\r') buf[count++] = c; // Игнорируем CR
    }
    buf[count] = 0;
    return count;
}

void ScriptManager::scriptTask(void* param) {
    ScriptManager* mgr = (ScriptManager*)param;
    File file = SD.open(mgr->_currentScriptPath);
    if (!file) { mgr->_running = false; vTaskDelete(NULL); }

    mgr->parseAndExecute(file);
    file.close();
    mgr->_running = false;
    vTaskDelete(NULL);
}

// Реализация соответствует хедеру (принимает char*)
ScriptLine ScriptManager::parseLine(char* line) {
    ScriptLine sl;
    sl.cmd = ScriptCmd::NONE;
    
    // Trim leading spaces
    char* ptr = line;
    while (*ptr == ' ') ptr++;
    if (*ptr == 0 || strncmp(ptr, "//", 2) == 0) return sl;

    // Split command and arg
    char* spacePos = strchr(ptr, ' ');
    if (spacePos) {
        *spacePos = 0; // Null terminate command
        sl.arg = String(spacePos + 1); // Arg can be String for convenience (short lived)
        sl.arg.trim();
    }
    
    String cmdStr = String(ptr);
    cmdStr.toUpperCase();

    if (cmdStr == "WAIT_RX") sl.cmd = ScriptCmd::WAIT_RX;
    else if (cmdStr == "IF_SIGNAL") { sl.cmd = ScriptCmd::IF_SIGNAL; sl.val = strtoul(sl.arg.c_str(), NULL, 16); }
    else if (cmdStr == "TX_RAW") sl.cmd = ScriptCmd::TX_RAW;
    else if (cmdStr == "LOG") sl.cmd = ScriptCmd::LOG_MSG;
    else if (cmdStr == "DELAY") { sl.cmd = ScriptCmd::DELAY_MS; sl.val = sl.arg.toInt(); }

    return sl;
}

void ScriptManager::parseAndExecute(File& file) {
    // FIX v6.3: Статический буфер вместо String для защиты кучи
    char lineBuf[128]; 
    
    while (file.available() && _running) {
        readLine(file, lineBuf, sizeof(lineBuf));
        ScriptLine sl = parseLine(lineBuf);

        switch (sl.cmd) {
            case ScriptCmd::WAIT_RX:
                SubGhzManager::getInstance().startCapture(); 
                _signalReceived = false;
                while (!_signalReceived && _running) vTaskDelay(10);
                break;
            case ScriptCmd::IF_SIGNAL:
                if (_signalReceived && _lastHash == sl.val) {} 
                else { 
                    // Пропускаем следующую строку (блок else)
                    if (file.available()) readLine(file, lineBuf, sizeof(lineBuf)); 
                }
                break;
            case ScriptCmd::TX_RAW:
                SubGhzManager::getInstance().playFlipperFile(sl.arg.c_str());
                while(SubGhzManager::getInstance().isReplaying()) vTaskDelay(10);
                break;
            case ScriptCmd::LOG_MSG:
                Serial.println(sl.arg);
                break;
            case ScriptCmd::DELAY_MS:
                vTaskDelay(sl.val);
                break;
            default: break;
        }
        // Даем время другим задачам
        vTaskDelay(1);
    }
}