#pragma once
#include "Common.h"
#include "Engines.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>

class BleManager : public IAttackEngine {
public:
    static BleManager& getInstance();
    
    void setup() override;
    bool loop(StatusMessage& statusOut) override;
    void stop() override;
    
    void startSpoof(BleSpoofType type);

private:
    BleManager();
    
    bool _isRunning;
    BleSpoofType _currentType;
    BLEAdvertising* _pAdvertising;
    uint32_t _packetsSent;
    
    // MAC Rotation Logic
    uint32_t _lastMacRotateTime;
    void rotateMacAddress();
    
    void setPayload(BleSpoofType type);
};