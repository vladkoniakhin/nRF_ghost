#pragma once
#include "Common.h"
#include "Engines.h"
#include "Config.h"
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <vector>

namespace NrfReg {
    constexpr uint8_t CONFIG = 0x00; constexpr uint8_t EN_AA = 0x01; constexpr uint8_t RF_CH = 0x05;
    constexpr uint8_t RF_SETUP = 0x06; constexpr uint8_t RPD = 0x09; constexpr uint8_t TX_ADDR = 0x10;
    constexpr uint8_t RX_ADDR_P0 = 0x0A; constexpr uint8_t SETUP_AW = 0x03; constexpr uint8_t SETUP_RETR = 0x04;
    constexpr uint8_t CMD_W_REGISTER = 0x20; constexpr uint8_t CMD_R_REGISTER = 0x00;
    constexpr uint8_t CMD_FLUSH_TX = 0xE1; constexpr uint8_t CMD_W_TX_PAYLOAD_NOACK = 0xB0;
}

struct DiscoveredMouse {
    uint8_t address[5];
    uint8_t channel;
    bool isMicrosoft;
};

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
    bool _isJamming, _isSweeping, _isAnalyzing, _isMouseJack, _isSniffing;
    uint8_t _targetChannel, _sweepChannel;
    uint32_t _packetsSent;
    uint32_t _mask_csn_a, _mask_ce_a, _mask_csn_b, _mask_ce_b;
    uint8_t _noiseBuffer[32];
    std::vector<DiscoveredMouse> _targets;
    int _currentTargetIndex;
    
    File _scriptFile;
    bool _scriptRunning;
    uint32_t _scriptDelayEnd;

    void initRadio(uint32_t csn_mask, uint32_t ce_mask);
    inline void selectRadio(uint32_t csn_mask);
    inline void deselectRadio(uint32_t csn_mask);
    inline void enableRadio(uint32_t ce_mask);
    inline void disableRadio(uint32_t ce_mask);
    void writeRegister(uint32_t csn_mask, uint8_t reg, uint8_t value);
    uint8_t readRegister(uint32_t csn_mask, uint8_t reg);
    void writeAddress(uint32_t csn_mask, uint8_t reg, const uint8_t* addr, uint8_t len);
    uint8_t getRPD(uint32_t csn_mask);
    void transmitNoise(uint32_t csn_mask, uint32_t ce_mask);
    void hopChannel(uint8_t ch);
    bool checkForPacket();
    void sendHidPacket(uint8_t mod, uint8_t key);
    void processScript();
    void openScript();
    uint8_t calcChecksum(uint8_t* payload, size_t len);
};