#include "SubGhzManager.h"
#include "System.h"
#include "Config.h"
#include <esp_task_wdt.h>

volatile uint16_t g_subGhzBuffer[Config::RAW_BUFFER_SIZE];
volatile size_t g_subGhzIndex = 0;
volatile uint32_t g_subGhzLastTime = 0;
volatile bool g_subGhzCaptureDone = false;

struct ScopedSpi {
    ScopedSpi() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(1000)); }
    ~ScopedSpi() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

void IRAM_ATTR SubGhzManager::isrHandler() {
    if (g_subGhzCaptureDone || g_subGhzIndex >= Config::RAW_BUFFER_SIZE) return;
    uint32_t now = micros(); 
    uint32_t d = now - g_subGhzLastTime; 
    g_subGhzLastTime = now;
    if (d > 50 && d < 100000) g_subGhzBuffer[g_subGhzIndex++] = (uint16_t)d;
}

SubGhzManager& SubGhzManager::getInstance() { static SubGhzManager i; return i; }
SubGhzManager::SubGhzManager() : _radio(nullptr), _module(nullptr), _isAnalyzing(false), _isJamming(false), _isCapturing(false), _isReplaying(false), _isRollingCode(false), _currentFreq(433.92) {}

void SubGhzManager::setup() {
    _module = new Module(Config::PIN_CC_CS, Config::PIN_CC_GDO0, RADIOLIB_NC);
    _radio = new CC1101(_module);
    ScopedSpi lock;
    if(lock.locked()) {
        if(_radio->begin(433.92) == RADIOLIB_ERR_NONE) { 
            _radio->setOutputPower(10); 
            _radio->setOOK(true); 
            _radio->standby(); 
        }
    }
}

void SubGhzManager::stop() {
    _isAnalyzing = false; _isJamming = false; _isCapturing = false; _isReplaying = false;
    detachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0));
    ScopedSpi lock;
    if(lock.locked() && _radio) _radio->standby();
}

bool SubGhzManager::analyzeSignal() {
    if(g_subGhzIndex < 20) return false;
    int uniqueCounts = 0; uint16_t uniqueTimings[16] = {0};
    for(size_t i = 1; i < g_subGhzIndex; i++) {
        bool found = false; 
        for(int j = 0; j < uniqueCounts; j++) {
            if(abs(g_subGhzBuffer[i] - uniqueTimings[j]) < 150) { found = true; break; }
        }
        if(!found) { 
            if(uniqueCounts < 16) uniqueTimings[uniqueCounts++] = g_subGhzBuffer[i]; 
            else return true; 
        }
    }
    return (uniqueCounts > 5);
}

void SubGhzManager::startCapture() {
    stop(); _isCapturing = true; _isRollingCode = false; g_subGhzIndex = 0; g_subGhzCaptureDone = false;
    ScopedSpi lock;
    if(lock.locked()) {
        _radio->setFrequency(433.92); _radio->setOOK(true); _radio->receiveDirect();
        g_subGhzLastTime = micros();
        attachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0), SubGhzManager::isrHandler, CHANGE);
    }
}

void SubGhzManager::startReplay() {
    if(g_subGhzIndex < 10 || _isRollingCode) return;
    stop(); _isReplaying = true;
    ScopedSpi lock;
    if(lock.locked()) {
        _radio->setFrequency(433.92); _radio->setOOK(true); _radio->transmitDirect();
        pinMode(Config::PIN_CC_GDO0, OUTPUT);
        
        // RAW Replay from RAM is fast enough to block interrupts entirely without much issue
        portDISABLE_INTERRUPTS();
        bool level = true; 
        for(size_t i = 1; i < g_subGhzIndex; i++) { 
            digitalWrite(Config::PIN_CC_GDO0, level); delayMicroseconds(g_subGhzBuffer[i]); level = !level; 
        }
        digitalWrite(Config::PIN_CC_GDO0, LOW);
        portENABLE_INTERRUPTS();
        
        pinMode(Config::PIN_CC_GDO0, INPUT); _radio->standby();
    }
    _isReplaying = false;
}

// --- OPTIMIZED FLIPPER PARSER (Fixes PERF-01) ---
void SubGhzManager::playFlipperFile(const char* path) {
    if (!SD.exists(path)) return;
    stop(); _isReplaying = true;

    ScopedSpi lock;
    if(lock.locked()) {
        File file = SD.open(path);
        if (file) {
            float targetFreq = 433.92;
            char line[128];
            long dataStartPos = 0;
            
            // 1. Header Scan
            while(file.available()) {
                long currPos = file.position();
                size_t len = file.readBytesUntil('\n', line, 127);
                line[len] = 0;
                
                if (strncmp(line, "Frequency:", 10) == 0) {
                    char* val = line + 10;
                    long freqRaw = atol(val);
                    if (freqRaw > 0) targetFreq = (float)freqRaw / 1000000.0;
                }
                
                if (strncmp(line, "RAW_Data:", 9) == 0) {
                    dataStartPos = currPos;
                    break;
                }
            }

            _radio->setFrequency(targetFreq);
            _radio->setOOK(true);
            _radio->transmitDirect();
            pinMode(Config::PIN_CC_GDO0, OUTPUT);

            // 2. Data Transmission
            file.seek(dataStartPos);
            bool inData = false;
            
            while (file.available()) {
                esp_task_wdt_reset(); // Prevent WDT Reset

                size_t len = file.readBytesUntil('\n', line, 127);
                line[len] = 0;

                if (strncmp(line, "RAW_Data:", 9) == 0) inData = true;
                
                if (inData) {
                    char* ptr = line;
                    if (strncmp(line, "RAW_Data:", 9) == 0) ptr += 9;
                    
                    char* token = strtok(ptr, " ");
                    
                    // FIX PERF-01: Don't block interrupts for the whole line.
                    // Instead, disable interrupts ONLY for the duration of the pulse.
                    // This introduces minimal jitter (<1us overhead) but keeps System alive.
                    
                    while (token != NULL) {
                        int32_t val = atoi(token);
                        if (val != 0) {
                            portDISABLE_INTERRUPTS();
                            if (val > 0) {
                                digitalWrite(Config::PIN_CC_GDO0, HIGH);
                                delayMicroseconds(val);
                            } else {
                                digitalWrite(Config::PIN_CC_GDO0, LOW);
                                delayMicroseconds(-val);
                            }
                            portENABLE_INTERRUPTS(); // Give WiFi stack a chance to breathe
                        }
                        token = strtok(NULL, " ");
                    }
                }
            }
            digitalWrite(Config::PIN_CC_GDO0, LOW);
            pinMode(Config::PIN_CC_GDO0, INPUT);
            _radio->standby();
            file.close();
        }
    }
    _isReplaying = false;
}

void SubGhzManager::startAnalyzer() { 
    stop(); _isAnalyzing = true; _currentFreq = 433.0; 
    ScopedSpi lock;
    if(lock.locked() && _radio) _radio->standby();
}

void SubGhzManager::startJammer() { 
    stop(); _isJamming = true; 
    ScopedSpi lock;
    if(lock.locked()) { _radio->setFrequency(433.92); _radio->transmitDirect(0); }
}

bool SubGhzManager::loop(StatusMessage& out) {
    if(_isCapturing) {
        out.state = SystemState::ANALYZING_SUBGHZ_RX;
        if(g_subGhzIndex > 10 && (micros() - g_subGhzLastTime > Config::SIGNAL_TIMEOUT_US)) {
            g_subGhzCaptureDone = true; stop();
            _isRollingCode = analyzeSignal();
            snprintf(out.logMsg, MAX_LOG_MSG, _isRollingCode ? "ROLLING CODE!" : "Fixed Code OK");
        } else snprintf(out.logMsg, MAX_LOG_MSG, "Rec: %d", g_subGhzIndex);
        out.rollingCodeDetected = _isRollingCode;
        return true;
    }
    if(_isReplaying) { out.state = SystemState::ATTACKING_SUBGHZ_TX; out.isReplaying = true; return true; }
    if(_isAnalyzing) {
        out.state = SystemState::ANALYZING_SUBGHZ_RX;
        ScopedSpi lock;
        if(lock.locked()) {
            _currentFreq += 0.05; 
            if(_currentFreq > 434.28) _currentFreq = 433.0;
            _radio->setFrequency(_currentFreq);
            int idx = (int)((_currentFreq - 433.0) * 100);
            if(idx >= 0 && idx < 128) out.spectrum[idx] = (uint8_t)(_radio->getRSSI() + 100);
            snprintf(out.logMsg, MAX_LOG_MSG, "%.2f MHz", _currentFreq);
        }
        return true;
    }
    if(_isJamming) {
        out.state = SystemState::ATTACKING_SUBGHZ_TX;
        ScopedSpi lock;
        if(lock.locked()) _radio->transmitDirect(0);
        snprintf(out.logMsg, MAX_LOG_MSG, "Jamming"); vTaskDelay(100);
        return true;
    }
    return false;
}