#pragma once
#include <Arduino.h>
#include <SD.h>
#include "Common.h"

// GhostScript Commands
enum class ScriptCmd { NONE, WAIT_RX, IF_SIGNAL, TX_RAW, LOG_MSG, DELAY_MS, STOP };

struct ScriptLine {
    ScriptCmd cmd;
    String arg;
    uint32_t val;
};

class ScriptManager {
public:
    static ScriptManager& getInstance();
    
    void init();
    void runScript(const char* path);
    void stop();
    
    // Callback from SubGhz when signal received
    void notifySignal(uint32_t signalHash);
    
    bool isRunning() const { return _running; }

private:
    ScriptManager();
    
    static void scriptTask(void* param);
    void parseAndExecute(File& file);
    
    // FIX v6.3: Изменена сигнатура для работы с char* (Memory Safety)
    ScriptLine parseLine(char* line); 
    
    TaskHandle_t _taskHandle;
    bool _running;
    volatile bool _signalReceived;
    volatile uint32_t _lastHash;
    
    char _currentScriptPath[64];
};