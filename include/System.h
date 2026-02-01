#pragma once
#include "Common.h"
#include "Engines.h"
#include "WiFiManager.h"
#include "BleManager.h"
#include "NrfManager.h"
#include "SubGhzManager.h"
#include "LedManager.h"      
#include "SettingsManager.h"
#include "AdminManager.h"

class SystemController {
public:
    static SystemController& getInstance();
    void init();
    void runWorkerLoop();
    bool sendCommand(CommandMessage cmd);
    bool getStatus(StatusMessage& msg);
    
    TargetAP getSelectedTarget() { return _selectedTarget; }
    void setSelectedTarget(TargetAP t) { _selectedTarget = t; }
    std::vector<TargetAP> getScanResults() { return _wifiEngine.getScanResults(); }

private:
    SystemController();
    
    QueueHandle_t _commandQueue;
    QueueHandle_t _statusQueue;
    SystemState _currentState;
    IAttackEngine* _activeEngine = nullptr;
    
    WiFiAttackManager _wifiEngine;
    TargetAP _selectedTarget;
    
    void processCommand(CommandMessage cmd);
    void stopCurrentTask();
    
    // --- v3.0: Протокол JSON для ПК-клиента ---
    void parseSerialJson(String& input);
    void sendJsonSuccess(const char* msg);
    void sendJsonError(const char* err);
    void sendJsonFileList(const char* path); // Опционально, если будете использовать
};