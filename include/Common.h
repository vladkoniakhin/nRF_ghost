#pragma once
#include <Arduino.h>

constexpr size_t MAX_SSID_LEN = 33;
constexpr size_t MAX_LOG_MSG = 64;
constexpr size_t SPECTRUM_CHANNELS = 128;

// Глобальные состояния системы (UI реагирует на них)
enum class SystemState {
    IDLE,
    SCANNING, 
    SCAN_COMPLETE, 
    SCAN_EMPTY,
    ATTACKING_WIFI_DEAUTH,
    ATTACKING_WIFI_SPAM, 
    ATTACKING_EVIL_TWIN,
    HANDSHAKE_CAPTURED,
    
    // Новые состояния для v7.1
    ATTACKING_BLE,
    ATTACKING_NRF,       // Jammer
    ATTACKING_MOUSEJACK, // Injection
    SNIFFING_NRF,
    ANALYZING_NRF,
    
    ATTACKING_SUBGHZ_TX,
    ANALYZING_SUBGHZ_RX,
    
    ADMIN_MODE,
    WEB_CLIENT_CONNECTED,
    SD_ERROR,
    SYS_ERROR,
    
    // Меню
    MENU_SELECT_BLE, 
    MENU_SELECT_NRF
};

enum class InputEvent { NONE, BTN_UP, BTN_DOWN, BTN_SELECT, BTN_BACK };

enum class SystemCommand {
    CMD_NONE,
    CMD_START_SCAN_WIFI, CMD_STOP_SCAN, CMD_SELECT_TARGET,
    CMD_START_DEAUTH, CMD_START_BEACON_SPAM, CMD_START_EVIL_TWIN,
    
    CMD_START_BLE_SPOOF, // Параметр: Тип (0=iOS, 1=Android, 2=Win)
    
    CMD_START_NRF_JAM, CMD_START_NRF_ANALYZER,
    CMD_START_MOUSEJACK, CMD_START_NRF_SNIFF,
    
    CMD_START_SUBGHZ_SCAN, CMD_START_SUBGHZ_JAM, CMD_START_SUBGHZ_RX, CMD_START_SUBGHZ_TX,
    
    CMD_START_ADMIN_MODE,
    CMD_STOP_ATTACK,
    CMD_SAVE_SETTINGS
};

// Структура для передачи статуса на экран
struct StatusMessage {
    SystemState state;
    int packetsSent;
    char logMsg[MAX_LOG_MSG];
    uint8_t spectrum[SPECTRUM_CHANNELS]; // Для анализатора
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

// Структура команды (от UI к Ядру)
struct CommandMessage {
    SystemCommand cmd;
    int param1;
};