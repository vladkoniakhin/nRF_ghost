#pragma once
#include "Common.h"
#include "Engines.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <vector>

// УДАЛЕНО: struct TargetAP
// Она уже определена в Common.h, который подключен выше.
// Повторное определение вызывало ошибку компиляции.

enum class WiFiState {
    IDLE, 
    SCANNING, 
    SCAN_COMPLETE, 
    SCAN_EMPTY_WAIT,
    ATTACKING_DEAUTH, 
    ATTACKING_BEACON, 
    ATTACKING_EVIL_TWIN
};

class WiFiAttackManager : public IAttackEngine {
public:
    WiFiAttackManager();
    
    // Переопределение методов из IAttackEngine
    void setup() override;
    bool loop(StatusMessage& statusOut) override;
    void stop() override;

    // Специфичные методы WiFi
    void startScan();
    void startDeauth(const TargetAP& target);
    void startBeaconSpam();
    void startEvilTwin(const TargetAP& target);
    
    std::vector<TargetAP> getScanResults();

private:
    WiFiState _state;
    TargetAP _currentTarget;
    uint32_t _lastPacketTime;
    uint32_t _packetsSent;
    uint32_t _scanEmptyTime;
    bool _capturedHandshake;
    
    // Буфер для пакетов (Deauth/Beacon)
    uint8_t _packetBuffer[128];
    
    // Внутренние методы сборки пакетов
    void buildDeauthPacket();
    void buildBeaconPacket(const char* ssid);
    
    // Статический callback для сниффера (ISR)
    static void snifferHandler(void* buf, wifi_promiscuous_pkt_type_t type);
    
    // Список SSID для спама
    const std::vector<const char*> _spamSSIDs = {
        "Free WiFi", "Loading...", "Virus.exe", "FBI Surveillance",
        "Skynet", "Trojan.win32", "Connect for Bitcoin", "No Internet",
        "Your Neighbor", "Searching...", "Error 404"
    };
};