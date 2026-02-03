#pragma once
#include "Common.h"
#include "Engines.h"
#include "Config.h"
#include <SPI.h>
#include <vector>

namespace NrfReg {
    constexpr uint8_t CONFIG = 0x00; constexpr uint8_t EN_AA = 0x01; constexpr uint8_t RF_CH = 0x05;
    constexpr uint8_t RF_SETUP = 0x06; constexpr uint8_t RPD = 0x09; constexpr uint8_t TX_ADDR = 0x10;
    constexpr uint8_t RX_ADDR_P0 = 0x0A; constexpr uint8_t SETUP_AW = 0x03; constexpr uint8_t SETUP_RETR = 0x04;
    constexpr uint8_t CMD_W_REGISTER = 0x20; constexpr uint8_t CMD_R_REGISTER = 0x00;
    constexpr uint8_t CMD_FLUSH_TX = 0xE1; constexpr uint8_t CMD_W_TX_PAYLOAD_NOACK = 0xB0;
}

// MouseJack / HID Injection Structures
struct HidPacket {
    uint8_t address[5];
    uint8_t payload[16]; // Typ. Logitech/Microsoft payloads are < 16 bytes
    uint8_t len;
};

struct KeyPress {
    uint8_t mod;
    uint8_t key;
    uint16_t delayMs;
};

// Injection State Machine
enum class MjState { IDLE, SEND_KEY, WAIT_RELEASE, SEND_RELEASE, WAIT_NEXT };

class NrfManager : public IAttackEngine {
public:
    static NrfManager& getInstance();
    
    void setup() override;
    bool loop(StatusMessage& statusOut) override;
    void stop() override;
    
    void startJamming(uint8_t channel);
    void startAnalyzer();
    void startSniffing();
    void startMouseJack(int targetIndex);

private:
    NrfManager();
    
    // Hardware Helpers
    inline void selectRadio(uint32_t csn_mask);
    inline void deselectRadio(uint32_t csn_mask);
    inline void enableRadio(uint32_t ce_mask);
    inline void disableRadio(uint32_t ce_mask);
    void writeRegister(uint32_t csn_mask, uint8_t reg, uint8_t value);
    uint8_t readRegister(uint32_t csn_mask, uint8_t reg);
    void initRadio(uint32_t csn_mask, uint32_t ce_mask);
    void transmitNoise(uint32_t csn_mask, uint32_t ce_mask);
    bool checkForPacket();
    
    // MouseJack Logic
    void preparePayload();
    void transmitHid(uint8_t mod, uint8_t key);
    uint8_t calcChecksum(uint8_t* payload, size_t len);
    
    // State
    bool _isJamming, _isSweeping, _isAnalyzing, _isMouseJack;
    uint8_t _targetChannel, _sweepChannel;
    uint32_t _packetsSent;
    
    uint32_t _mask_csn_a, _mask_ce_a; // Only Radio A used for now
    uint8_t _noiseBuffer[32];
    
    // MouseJack Execution State
    std::vector<KeyPress> _duckyScript;
    size_t _scriptIdx;
    MjState _mjState;
    uint32_t _mjTimer;
    uint8_t _targetAddr[5];
};