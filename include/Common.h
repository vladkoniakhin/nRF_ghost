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
    MENU_SELECT_BLE, MENU_SELECT_NRF,
    SCANNING, SCAN_COMPLETE, SCAN_EMPTY,
    ATTACKING_WIFI, ATTACKING_BEACON, ATTACKING_EVIL_TWIN,
    ATTACKING_BLE,
    ATTACKING_NRF, ATTACKING_MOUSEJACK, SNIFFING_NRF,
    ATTACKING_SUBGHZ, ANALYZING_SUBGHZ,
    ANALYZING_NRF,
    ADMIN_MODE,
    PC_CLIENT_MODE,
    SYS_ERROR 
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