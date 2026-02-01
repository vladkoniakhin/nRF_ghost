#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <vector>

constexpr size_t MAX_SSID_LEN = 33;
constexpr size_t MAX_LOG_MSG = 64;
constexpr size_t SPECTRUM_CHANNELS = 128;

enum class SystemState {
    IDLE,
    
    // Menus
    MENU_SELECT_BLE, 
    MENU_SELECT_NRF,
    
    // WiFi States
    SCANNING, 
    SCAN_COMPLETE, 
    SCAN_EMPTY,
    ATTACKING_WIFI_DEAUTH,  // New: Specific Red Strobe
    ATTACKING_WIFI_SPAM,    // New: Yellow Blink
    ATTACKING_EVIL_TWIN,    // New: Purple Static
    HANDSHAKE_CAPTURED,     // New: Rainbow
    
    // Web
    ADMIN_MODE,
    WEB_CLIENT_CONNECTED,   // New: Cyan Breathe
    
    // BLE
    ATTACKING_BLE,
    
    // NRF
    ATTACKING_NRF,          // Jamming
    ATTACKING_MOUSEJACK,    // New: Red Flash
    SNIFFING_NRF,           // New: White Dim
    ANALYZING_NRF,
    
    // Sub-GHz
    ATTACKING_SUBGHZ_TX,    // New: Orange Static
    ANALYZING_SUBGHZ_RX,    // New: Blue Blink
    
    // System
    PC_CLIENT_MODE,
    SYS_ERROR,
    SD_ERROR                // New: Brick Red
};

enum class InputEvent { NONE, BTN_UP, BTN_DOWN, BTN_SELECT, BTN_BACK };

enum class SystemCommand {
    CMD_NONE,
    CMD_START_SCAN_WIFI, CMD_STOP_SCAN, CMD_SELECT_TARGET,
    CMD_START_DEAUTH, CMD_START_BEACON_SPAM, CMD_START_EVIL_TWIN,
    CMD_START_BLE_SPOOF,
    CMD_START_NRF_JAM, CMD_START_NRF_ANALYZER,
    CMD_START_MOUSEJACK, CMD_START_NRF_SNIFF,
    CMD_START_SUBGHZ_SCAN, CMD_START_SUBGHZ_JAM, CMD_START_SUBGHZ_RX, CMD_START_SUBGHZ_TX,
    CMD_START_ADMIN_MODE,
    CMD_STOP_ATTACK,
    CMD_SAVE_SETTINGS
};

enum class BleSpoofType { APPLE_AIRPODS, ANDROID_FASTPAIR, WINDOWS_SWIFT };

struct DeviceSettings {
    bool ledEnabled = true;
    uint8_t brightness = 255;
    uint8_t defaultChannel = 1;
};

struct CommandMessage {
    SystemCommand cmd;
    int param1;
};

struct StatusMessage {
    SystemState state;
    int packetsSent;
    char logMsg[MAX_LOG_MSG];
    uint8_t spectrum[SPECTRUM_CHANNELS];
    bool handshakeCaptured;
    bool isReplaying;
    bool rollingCodeDetected;
};

struct TargetAP {
    char ssid[MAX_SSID_LEN];
    uint8_t bssid[6];
    uint8_t channel;
    int rssi;
};

struct CapturedPacket {
    uint32_t timestamp;
    uint16_t length;
    uint8_t data[256];
};