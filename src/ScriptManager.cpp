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
        // Allow some time for graceful exit if needed, but since this task
        // is reading line by line, _running=false breaks the loop instantly.
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

void ScriptManager::scriptTask(void* param) {
    ScriptManager* mgr = (ScriptManager*)param;
    File file = SD.open(mgr->_currentScriptPath);
    if (!file) { mgr->_running = false; vTaskDelete(NULL); }

    mgr->parseAndExecute(file);
    file.close();
    mgr->_running = false;
    vTaskDelete(NULL);
}

ScriptLine ScriptManager::parseLine(String line) {
    ScriptLine sl;
    sl.cmd = ScriptCmd::NONE;
    line.trim();
    if (line.length() == 0 || line.startsWith("//")) return sl;

    int spaceIdx = line.indexOf(' ');
    String cmdStr = (spaceIdx == -1) ? line : line.substring(0, spaceIdx);
    
    if (spaceIdx != -1) {
        sl.arg = line.substring(spaceIdx + 1);
        sl.arg.trim();
        // Remove quotes if present
        if (sl.arg.startsWith("\"") && sl.arg.endsWith("\"")) {
            sl.arg = sl.arg.substring(1, sl.arg.length() - 1);
        } else if (sl.arg.startsWith("'") && sl.arg.endsWith("'")) {
            sl.arg = sl.arg.substring(1, sl.arg.length() - 1);
        }
    }

    cmdStr.toUpperCase();

    if (cmdStr == "WAIT_RX") sl.cmd = ScriptCmd::WAIT_RX;
    else if (cmdStr == "IF_SIGNAL") { sl.cmd = ScriptCmd::IF_SIGNAL; sl.val = strtoul(sl.arg.c_str(), NULL, 16); }
    else if (cmdStr == "TX_RAW") sl.cmd = ScriptCmd::TX_RAW;
    else if (cmdStr == "LOG") sl.cmd = ScriptCmd::LOG_MSG;
    else if (cmdStr == "DELAY") { sl.cmd = ScriptCmd::DELAY_MS; sl.val = sl.arg.toInt(); }

    return sl;
}

void ScriptManager::parseAndExecute(File& file) {
    while (file.available() && _running) {
        String line = file.readStringUntil('\n');
        ScriptLine sl = parseLine(line);

        switch (sl.cmd) {
            case ScriptCmd::WAIT_RX:
                SubGhzManager::getInstance().startCapture(); 
                _signalReceived = false;
                while (!_signalReceived && _running) vTaskDelay(10);
                break;
            case ScriptCmd::IF_SIGNAL:
                if (_signalReceived && _lastHash == sl.val) {} 
                else { if (file.available()) file.readStringUntil('\n'); }
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
    }
}