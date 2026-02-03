#include "SubGhzManager.h"
#include "System.h"
#include "Config.h"
#include "ScriptManager.h"
#include <esp_task_wdt.h>
#include <FS.h>
#include <SD.h>
#include <driver/rmt.h>
#include <vector>

// --- HARDWARE CONSTANTS ---
#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV 80  // 80MHz / 80 = 1MHz (1 tick = 1 us)
#define MAX_CAPTURE_SIZE_ITEMS 20000 // Лимит RAM (~80KB)

// Globals
volatile uint16_t g_subGhzBuffer[Config::RAW_BUFFER_SIZE]; 
volatile size_t g_subGhzIndex = 0;
volatile uint32_t g_subGhzLastTime = 0;
volatile bool g_subGhzCaptureDone = false;

static char g_playbackFilePath[64];

// --- HELPERS ---

void saveLastCapture() {
    if (g_subGhzIndex < 10) return; 
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(500))) {
        if (SD.exists("/last_capture.sub")) SD.remove("/last_capture.sub");
        File f = SD.open("/last_capture.sub", FILE_WRITE);
        if (f) {
            f.println("Filetype: Flipper SubGhz RAW File");
            f.println("Version: 1");
            f.println("Frequency: 433920000");
            f.println("Preset: FuriHalSubGhzPresetOok650Async");
            f.println("Protocol: RAW");
            f.print("RAW_Data: ");
            for(size_t i=0; i<g_subGhzIndex; i++) {
                f.print(g_subGhzBuffer[i]);
                f.print(" ");
                if (i > 0 && i % 20 == 0) f.print("\nRAW_Data: ");
            }
            f.println();
            f.close();
        }
        xSemaphoreGive(g_spiMutex);
    }
}

struct SubGhzLock {
    SubGhzLock() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(1000)); }
    ~SubGhzLock() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

// ISR Handler (RX)
void IRAM_ATTR SubGhzManager::isrHandler() {
    if (g_subGhzCaptureDone || g_subGhzIndex >= Config::RAW_BUFFER_SIZE) return;
    uint32_t now = micros(); 
    uint32_t d = now - g_subGhzLastTime; 
    g_subGhzLastTime = now;
    if (d > 50) { 
        if (d > 65535) d = 65535; 
        g_subGhzBuffer[g_subGhzIndex++] = (uint16_t)d; 
    }
}

SubGhzManager& SubGhzManager::getInstance() { static SubGhzManager i; return i; }

SubGhzManager::SubGhzManager() : 
    _radio(nullptr), _module(nullptr), 
    _isAnalyzing(false), _isJamming(false), _isCapturing(false), 
    _isReplaying(false), _isBruteForcing(false), _isRollingCode(false), 
    _currentFreq(433.92), _currentModulation(Modulation::OOK),
    _shouldStop(false), _producerTaskHandle(nullptr)
{ 
    _rmtQueue = xQueueCreate(10, sizeof(RmtBlock)); 
}

bool SubGhzManager::isReplaying() const { return _isReplaying || _isBruteForcing; }

void SubGhzManager::configureRmt() {
    rmt_config_t config;
    config.rmt_mode = RMT_MODE_TX;
    config.channel = RMT_TX_CHANNEL;
    config.gpio_num = (gpio_num_t)Config::PIN_CC_GDO0;
    config.mem_block_num = 1;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false; 
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    config.clk_div = RMT_CLK_DIV; 

    rmt_config(&config);
    rmt_driver_install(config.channel, 0, 0);
}

void SubGhzManager::setup() {
    _module = new Module(Config::PIN_CC_CS, Config::PIN_CC_GDO0, RADIOLIB_NC);
    _radio = new CC1101(_module);
    SubGhzLock lock;
    if(lock.locked()) { 
        if(_radio->begin(433.92) == RADIOLIB_ERR_NONE) { 
            _radio->setOutputPower(10); 
            _radio->setOOK(true); 
            _radio->standby(); 
        } 
    }
    configureRmt();
}

void SubGhzManager::stop() {
    bool wasCapturing = _isCapturing;
    _isAnalyzing = false; _isJamming = false; 
    _isCapturing = false; _isReplaying = false; _isBruteForcing = false;
    
    detachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0));
    
    if (_producerTaskHandle != nullptr) {
        _shouldStop = true;
        uint32_t start = millis();
        while (_producerTaskHandle != nullptr && millis() - start < 1000) vTaskDelay(10);
        if (_producerTaskHandle != nullptr) { vTaskDelete(_producerTaskHandle); _producerTaskHandle = nullptr; }
    }
    _shouldStop = false; 
    xQueueReset(_rmtQueue);
    
    SubGhzLock lock; 
    if(lock.locked() && _radio) _radio->standby();
    
    if (wasCapturing && g_subGhzIndex > 10) saveLastCapture();
}

void SubGhzManager::setModulation(Modulation mod, float dev) {
    if (mod == _currentModulation) return;
    if (mod == Modulation::OOK) { _radio->setOOK(true); } 
    else { _radio->setOOK(false); _radio->setFrequencyDeviation(dev); }
    _currentModulation = mod;
}

// --- SAFE FILE READING ---
size_t readLineSafely(File& f, char* buf, size_t maxLen) {
    size_t count = 0;
    while (f.available() && count < maxLen - 1) {
        char c = f.read();
        if (c == '\n') break;
        if (c != '\r') buf[count++] = c;
    }
    buf[count] = 0; return count;
}

// --- PRODUCTION GRADE REPLAY TASK (Load-to-RAM) ---
void SubGhzManager::producerTask(void* param) {
    SubGhzManager* mgr = (SubGhzManager*)param;
    float freq = 433.92; float deviation = 0.0; Modulation mod = Modulation::OOK; long dataStart = 0;
    char line[128]; 

    // 1. HEADER PARSING
    {
        SubGhzLock lock;
        if (!lock.locked()) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }
        File file = SD.open(g_playbackFilePath);
        if (!file) { Serial.println("[SubGhz] File Not Found"); mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }

        while(file.available()) {
            readLineSafely(file, line, 128);
            if (strncmp(line, "Frequency:", 10) == 0) freq = (float)atol(line + 10) / 1000000.0;
            if (strncmp(line, "Preset:", 7) == 0) { if (strstr(line, "FSK")) { mod = Modulation::FSK2; deviation = Config::FSK_DEVIATION_DEFAULT; } else { mod = Modulation::OOK; } }
            if (strncmp(line, "RAW_Data:", 9) == 0) { dataStart = file.position(); break; }
        }
        file.close(); 
    }

    // 2. LOAD TO RAM (PRE-CACHING)
    std::vector<rmt_item32_t> ramBuffer;
    
    // Check Heap before allocation
    if (ESP.getFreeHeap() < 20000) {
        Serial.println("[SubGhz] Not enough RAM!");
        mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return;
    }

    {
        SubGhzLock lock;
        if (!lock.locked()) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }
        File file = SD.open(g_playbackFilePath);
        file.seek(dataStart);
        
        Serial.println("[SubGhz] Loading to RAM...");
        
        while(true) {
            if (!file.available()) break;
            readLineSafely(file, line, 128);
            
            // Limit check
            if (ramBuffer.size() > MAX_CAPTURE_SIZE_ITEMS) {
                Serial.println("[SubGhz] File too large!");
                break;
            }

            if (strncmp(line, "RAW_Data:", 9) == 0) {
                char* ptr = line + 9;
                char* token = strtok(ptr, " ");
                while (token != NULL) {
                    int32_t val = atoi(token);
                    if (val != 0) {
                        uint32_t duration = abs(val); 
                        uint8_t level = (val > 0) ? 1 : 0;
                        
                        while (duration > 32767) {
                            ramBuffer.push_back({{{32767, level, 32767, level}}}); 
                            duration -= 65534;
                        }
                        if (duration > 0) {
                            ramBuffer.push_back({{{ (uint16_t)duration, level, 0, 0 }}});
                        }
                    }
                    token = strtok(NULL, " ");
                }
            }
            if (ramBuffer.size() % 100 == 0) esp_task_wdt_reset();
        }
        file.close();
    }
    
    Serial.printf("[SubGhz] Loaded %d items. Starting TX.\n", ramBuffer.size());

    // 3. HARDWARE SETUP
    { 
        SubGhzLock lock; 
        if(lock.locked()) {
            mgr->_radio->setFrequency(freq); 
            mgr->setModulation(mod, deviation); 
            mgr->_radio->transmitDirect(); 
        }
    }

    // 4. ATOMIC TRANSMISSION FROM RAM
    size_t itemsSent = 0;
    const size_t CHUNK = 64; 
    
    while(itemsSent < ramBuffer.size()) {
        if (mgr->_shouldStop) break;
        
        size_t remaining = ramBuffer.size() - itemsSent;
        size_t toSend = (remaining > CHUNK) ? CHUNK : remaining;
        
        rmt_write_items(RMT_TX_CHANNEL, &ramBuffer[itemsSent], toSend, true);
        rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
        
        itemsSent += toSend;
    }

    // Cleanup
    { SubGhzLock lock; if(lock.locked()) mgr->_radio->standby(); }
    ramBuffer.clear(); 
    mgr->_producerTaskHandle = nullptr; 
    vTaskDelete(NULL);
}

void SubGhzManager::playFlipperFile(const char* path) {
    if (!SD.exists(path)) return; stop(); _isReplaying = true;
    strncpy(g_playbackFilePath, path, 63); _shouldStop = false;
    xTaskCreatePinnedToCore(producerTask, "SubGhzProd", Config::SUBGHZ_STACK_SIZE, this, 1, &_producerTaskHandle, 1);
}

// Simple BruteForce (CPU Driven as it is algorithmic)
void SubGhzManager::bruteForceTask(void* param) {
    SubGhzManager* mgr = (SubGhzManager*)param;
    { SubGhzLock lock; if (!lock.locked()) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }
      mgr->_radio->setFrequency(433.92); mgr->_radio->setOOK(true); mgr->_radio->transmitDirect(); }

    uint16_t Te = Config::CAME_BIT_PERIOD;
    rmt_item32_t buf[32];

    for (uint16_t code = 0; code < 4096; code++) {
        if (mgr->_shouldStop) break;
        esp_task_wdt_reset();
        
        int idx = 0;
        buf[idx++] = {{{ (uint16_t)Te, 1, (uint16_t)(36*Te), 0 }}}; // Start

        for (int b = 11; b >= 0; b--) {
            bool bit = (code >> b) & 1;
            if (bit == 0) buf[idx++] = {{{ (uint16_t)Te, 1, (uint16_t)(2*Te), 0 }}};
            else buf[idx++] = {{{ (uint16_t)(2*Te), 1, (uint16_t)Te, 0 }}};
        }
        buf[idx++] = {{{ 0, 0, (uint16_t)(36*Te), 0 }}}; // Pause

        rmt_write_items(RMT_TX_CHANNEL, buf, idx, true);
        rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
    }
    mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL);
}

void SubGhzManager::startBruteForce() {
    stop(); _isBruteForcing = true; _shouldStop = false;
    xTaskCreatePinnedToCore(bruteForceTask, "BruteForce", Config::SUBGHZ_STACK_SIZE, this, 1, &_producerTaskHandle, 1);
}

void SubGhzManager::startCapture() { stop(); _isCapturing=true; g_subGhzIndex=0; g_subGhzCaptureDone=false; SubGhzLock l; if(l.locked()) { _radio->setFrequency(433.92); _radio->setOOK(true); _radio->receiveDirect(); attachInterrupt(Config::PIN_CC_GDO0, isrHandler, CHANGE); }}
void SubGhzManager::startAnalyzer() { stop(); _isAnalyzing=true; _currentFreq=433.0; SubGhzLock l; if(l.locked()) _radio->standby(); }
void SubGhzManager::startJammer() { stop(); _isJamming=true; SubGhzLock l; if(l.locked()) { _radio->setFrequency(433.92); _radio->transmitDirect(0); }}

bool SubGhzManager::loop(StatusMessage& out) {
    if (_isReplaying || _isBruteForcing) {
        out.state = SystemState::ATTACKING_SUBGHZ_TX; 
        out.isReplaying = true;
        if (_producerTaskHandle == nullptr) { 
             _isReplaying = false; _isBruteForcing = false; 
             stop(); 
             snprintf(out.logMsg, MAX_LOG_MSG, "TX Done"); 
        } else {
             snprintf(out.logMsg, MAX_LOG_MSG, "TX Running..."); 
        }
        return true;
    }
    if(_isCapturing) {
        out.state = SystemState::ANALYZING_SUBGHZ_RX;
        if(g_subGhzIndex > 10 && (micros() - g_subGhzLastTime > Config::SIGNAL_TIMEOUT_US)) {
            g_subGhzCaptureDone = true; stop(); _isRollingCode = analyzeSignal();
            if (g_subGhzIndex > 20) { uint32_t hash = 0; for(size_t i=0; i<g_subGhzIndex; i++) hash += g_subGhzBuffer[i]; ScriptManager::getInstance().notifySignal(hash); }
            snprintf(out.logMsg, MAX_LOG_MSG, _isRollingCode ? "ROLLING CODE!" : "Fixed Code OK");
        } else snprintf(out.logMsg, MAX_LOG_MSG, "Rec: %d", g_subGhzIndex);
        out.rollingCodeDetected = _isRollingCode; return true;
    }
    if(_isAnalyzing) {
        out.state = SystemState::ANALYZING_SUBGHZ_RX; SubGhzLock lock;
        if(lock.locked()) {
            _currentFreq += 0.05; if(_currentFreq > 434.28) _currentFreq = 433.0; _radio->setFrequency(_currentFreq);
            int idx = (int)((_currentFreq - 433.0) * 100); if(idx >= 0 && idx < 128) out.spectrum[idx] = (uint8_t)(_radio->getRSSI() + 100);
            snprintf(out.logMsg, MAX_LOG_MSG, "%.2f MHz", _currentFreq);
        }
        return true;
    }
    if(_isJamming) {
        out.state = SystemState::ATTACKING_SUBGHZ_TX; SubGhzLock lock;
        if(lock.locked()) _radio->transmitDirect(0);
        snprintf(out.logMsg, MAX_LOG_MSG, "Jamming"); vTaskDelay(100);
        return true;
    }
    return false;
}

bool SubGhzManager::analyzeSignal() {
    if(g_subGhzIndex < 20) return false;
    int u=0; uint16_t k[16]={0};
    for(size_t i=1;i<g_subGhzIndex;i++) {
        bool m=false; for(int j=0;j<u;j++) if(abs(g_subGhzBuffer[i]-k[j])<150) { m=true; break; }
        if(!m) { if(u<16) k[u++]=g_subGhzBuffer[i]; else return true; }
    }
    return (u > 5);
}