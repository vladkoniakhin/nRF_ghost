#include "NrfManager.h"
#include "System.h"

static const uint8_t ATTACK_ADDRESS[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
#define KEY_MOD_LCTRL  0x01
#define KEY_MOD_LMETA  0x08 

NrfManager& NrfManager::getInstance() { static NrfManager instance; return instance; }

NrfManager::NrfManager()
    : _isJamming(false), _isSweeping(false), _isAnalyzing(false), _isMouseJack(false), _isSniffing(false),
      _targetChannel(1), _sweepChannel(1), _packetsSent(0), _scriptRunning(false), _scriptDelayEnd(0)
{
    for(int i=0; i<32; i++) _noiseBuffer[i] = (uint8_t)random(0, 255);
    auto getMask = [](uint8_t pin) -> uint32_t { return (1 << pin); };
    _mask_csn_a = getMask(Config::PIN_NRF_CSN_A); _mask_ce_a = getMask(Config::PIN_NRF_CE_A);
    _mask_csn_b = getMask(Config::PIN_NRF_CSN_B); _mask_ce_b = getMask(Config::PIN_NRF_CE_B);
}

inline void NrfManager::selectRadio(uint32_t csn_mask) { GPIO.out_w1tc = csn_mask; }
inline void NrfManager::deselectRadio(uint32_t csn_mask) { GPIO.out_w1ts = csn_mask; }
inline void NrfManager::enableRadio(uint32_t ce_mask) { GPIO.out_w1ts = ce_mask; }
inline void NrfManager::disableRadio(uint32_t ce_mask) { GPIO.out_w1tc = ce_mask; }

void NrfManager::writeRegister(uint32_t csn_mask, uint8_t reg, uint8_t value) {
    selectRadio(csn_mask);
    SPI.transfer(NrfReg::CMD_W_REGISTER | (reg & 0x1F));
    SPI.transfer(value);
    deselectRadio(csn_mask);
}

uint8_t NrfManager::readRegister(uint32_t csn_mask, uint8_t reg) {
    selectRadio(csn_mask);
    SPI.transfer(NrfReg::CMD_R_REGISTER | (reg & 0x1F));
    uint8_t result = SPI.transfer(0x00);
    deselectRadio(csn_mask);
    return result;
}

void NrfManager::initRadio(uint32_t csn_mask, uint32_t ce_mask) {
    disableRadio(ce_mask);
    deselectRadio(csn_mask);
    delay(5);
    writeRegister(csn_mask, NrfReg::CONFIG, 0x0E); 
    writeRegister(csn_mask, NrfReg::EN_AA, 0x00); 
    writeRegister(csn_mask, NrfReg::RF_SETUP, 0x0F); 
    writeRegister(csn_mask, NrfReg::SETUP_AW, 0x03); 
    writeRegister(csn_mask, NrfReg::SETUP_RETR, 0x00); 
    writeRegister(csn_mask, NrfReg::RF_CH, 2);
}

void NrfManager::setup() {
    pinMode(Config::PIN_NRF_CSN_A, OUTPUT); pinMode(Config::PIN_NRF_CE_A, OUTPUT);
    pinMode(Config::PIN_NRF_CSN_B, OUTPUT); pinMode(Config::PIN_NRF_CE_B, OUTPUT);
    digitalWrite(Config::PIN_NRF_CSN_A, HIGH); digitalWrite(Config::PIN_NRF_CE_A, LOW);
    digitalWrite(Config::PIN_NRF_CSN_B, HIGH); digitalWrite(Config::PIN_NRF_CE_B, LOW);

    // FIX v6.4: Increased timeout for Init from 10 to 100ms
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(100))) {
        SPI.begin(Config::PIN_SPI_SCK, Config::PIN_SPI_MISO, Config::PIN_SPI_MOSI);
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        initRadio(_mask_csn_a, _mask_ce_a);
        SPI.endTransaction();
        xSemaphoreGive(g_spiMutex);
    }
}

void NrfManager::stop() {
    _isJamming = false; _isSweeping = false; _isAnalyzing = false; _isMouseJack = false; _isSniffing = false;
    disableRadio(_mask_ce_a); disableRadio(_mask_ce_b);
    
    // FIX v6.4: Graceful shutdown with SPI lock
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        writeRegister(_mask_csn_a, NrfReg::CONFIG, 0x00); // Power Down
        SPI.endTransaction();
        xSemaphoreGive(g_spiMutex);
    }
}

void NrfManager::transmitNoise(uint32_t csn_mask, uint32_t ce_mask) {
    selectRadio(csn_mask);
    SPI.transfer(NrfReg::CMD_W_TX_PAYLOAD_NOACK);
    SPI.writeBytes(_noiseBuffer, 32);
    deselectRadio(csn_mask);
    enableRadio(ce_mask);
    delayMicroseconds(Config::NRF_JAMMING_DELAY_US);
    disableRadio(ce_mask);
}

bool NrfManager::checkForPacket() { return (readRegister(_mask_csn_a, 0x09) & 1); } 

void NrfManager::startJamming(uint8_t channel) { stop(); _isJamming = true; _targetChannel = channel; }
void NrfManager::startAnalyzer() { stop(); _isAnalyzing = true; }
void NrfManager::startSniffing() { stop(); _isSweeping = true; _targets.clear(); }
void NrfManager::startMouseJack(int targetIndex) { stop(); _isMouseJack = true; _currentTargetIndex = targetIndex; openScript(); }

void NrfManager::openScript() { _scriptRunning = true; }
void NrfManager::processScript() { _scriptRunning = false; }

bool NrfManager::loop(StatusMessage& statusOut) {
    if (_isJamming) {
        statusOut.state = SystemState::ATTACKING_NRF;
        // FIX v6.4: Timeout 50ms - позволяет делить шину с SD
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
            SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
            writeRegister(_mask_csn_a, NrfReg::RF_CH, _targetChannel);
            transmitNoise(_mask_csn_a, _mask_ce_a);
            SPI.endTransaction();
            xSemaphoreGive(g_spiMutex);
        }
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Jamming Ch: %d", _targetChannel);
        return true;
    }
    if (_isSweeping) {
        statusOut.state = SystemState::SNIFFING_NRF;
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
            SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
            _sweepChannel++; if(_sweepChannel > 80) _sweepChannel = 2;
            writeRegister(_mask_csn_a, NrfReg::RF_CH, _sweepChannel); disableRadio(_mask_ce_a); enableRadio(_mask_ce_a); delayMicroseconds(500); 
            if (checkForPacket()) snprintf(statusOut.logMsg, MAX_LOG_MSG, "Found: %d Devices", (int)_targets.size());
            else snprintf(statusOut.logMsg, MAX_LOG_MSG, "Scanning Ch: %d", _sweepChannel);
            SPI.endTransaction(); xSemaphoreGive(g_spiMutex);
        }
        return true;
    }
    if (_isMouseJack) {
        statusOut.state = SystemState::ATTACKING_MOUSEJACK;
        if (_scriptRunning) processScript();
        snprintf(statusOut.logMsg, MAX_LOG_MSG, _scriptRunning ? "Injecting..." : "Done.");
        vTaskDelay(20);
        return true;
    }
    if (_isAnalyzing) {
        statusOut.state = SystemState::ANALYZING_NRF;
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
            SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
            for (int ch = 0; ch < SPECTRUM_CHANNELS; ch++) {
               statusOut.spectrum[ch] = 0; 
            }
            snprintf(statusOut.logMsg, MAX_LOG_MSG, "2.4GHz Analyzer");
            SPI.endTransaction(); xSemaphoreGive(g_spiMutex);
        }
        return true;
    }
    return false;
}