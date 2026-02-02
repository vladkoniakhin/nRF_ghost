#include "SubGhzManager.h"
#include "System.h"
#include "Config.h"
#include "ScriptManager.h"
#include <esp_task_wdt.h>

// FIX: Переменные в IRAM (DRAM) для быстрого доступа из ISR
// __attribute__((section(".noinit"))) или просто volatile
// Главное - они не должны быть в PSRAM (если она есть)
volatile uint16_t g_subGhzBuffer[Config::RAW_BUFFER_SIZE]; 
volatile size_t g_subGhzIndex = 0;
volatile uint32_t g_subGhzLastTime = 0;
volatile bool g_subGhzCaptureDone = false;

static char g_playbackFilePath[64];

#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV 80 

struct SubGhzLock {
    SubGhzLock() { _ok = xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(1000)); }
    ~SubGhzLock() { if (_ok) xSemaphoreGive(g_spiMutex); }
    bool locked() { return _ok; }
    bool _ok;
};

// FIX: IRAM_ATTR для обработчика прерываний
void IRAM_ATTR SubGhzManager::isrHandler() {
    if (g_subGhzCaptureDone || g_subGhzIndex >= Config::RAW_BUFFER_SIZE) return;
    
    uint32_t now = micros(); 
    uint32_t d = now - g_subGhzLastTime; 
    g_subGhzLastTime = now;
    
    // FIX: Безопасное приведение типов (не кастить > 65535)
    if (d > 50) {
        if (d > 65535) d = 65535; // Clamp value
        g_subGhzBuffer[g_subGhzIndex++] = (uint16_t)d;
    }
}

SubGhzManager& SubGhzManager::getInstance() { static SubGhzManager i; return i; }

SubGhzManager::SubGhzManager() : 
    _radio(nullptr), _module(nullptr), 
    _isAnalyzing(false), _isJamming(false), _isCapturing(false), 
    _isReplaying(false), _isBruteForcing(false), 
    _isRollingCode(false), _currentFreq(433.92), _currentModulation(Modulation::OOK),
    _shouldStop(false),
    _producerTaskHandle(nullptr)
{
    _rmtQueue = xQueueCreate(10, sizeof(RmtBlock));
}

bool SubGhzManager::isReplaying() const { return _isReplaying || _isBruteForcing; }

void SubGhzManager::configureRmt() {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)Config::PIN_CC_GDO0, RMT_TX_CHANNEL);
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
            _radio->setOutputPower(10); _radio->setOOK(true); _radio->standby(); 
        }
    }
    configureRmt();
}

void SubGhzManager::stop() {
    _isAnalyzing = false; _isJamming = false; 
    _isCapturing = false; _isReplaying = false; _isBruteForcing = false;
    
    detachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0));
    
    if (_producerTaskHandle != nullptr) {
        _shouldStop = true;
        uint32_t start = millis();
        // Ждем пока задача сама выйдет
        while (_producerTaskHandle != nullptr && millis() - start < 1000) {
            vTaskDelay(10);
        }
        // Если зависла - убиваем
        if (_producerTaskHandle != nullptr) {
            vTaskDelete(_producerTaskHandle);
            _producerTaskHandle = nullptr;
        }
    }
    _shouldStop = false;
    xQueueReset(_rmtQueue);

    SubGhzLock lock;
    if(lock.locked() && _radio) _radio->standby();
}

void SubGhzManager::setModulation(Modulation mod, float dev) {
    if (mod == _currentModulation) return;
    if (mod == Modulation::OOK) { _radio->setOOK(true); } 
    else { _radio->setOOK(false); _radio->setFrequencyDeviation(dev); }
    _currentModulation = mod;
}

// --- BRUTE FORCE TASK ---
void SubGhzManager::bruteForceTask(void* param) {
    SubGhzManager* mgr = (SubGhzManager*)param;
    
    {
        SubGhzLock lock;
        if (!lock.locked()) { 
            mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; 
        }
        mgr->_radio->setFrequency(433.92);
        mgr->_radio->setOOK(true);
        mgr->_radio->transmitDirect();
    }

    uint16_t Te = Config::CAME_BIT_PERIOD;
    
    for (uint16_t code = 0; code < 4096; code++) {
        if (mgr->_shouldStop) break;
        esp_task_wdt_reset();

        RmtBlock block;
        block.itemCount = 0;
        
        for (int b = 11; b >= 0; b--) {
            bool bit = (code >> b) & 1;
            if (bit == 0) block.items[block.itemCount++] = {{{ (uint16_t)Te, 1, (uint16_t)(2*Te), 0 }}};
            else block.items[block.itemCount++] = {{{ (uint16_t)(2*Te), 1, (uint16_t)Te, 0 }}};
        }
        block.items[block.itemCount++] = {{{ 0, 0, (uint16_t)(36*Te), 0 }}};
        
        // Timeout вместо бесконечного ожидания
        if (xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (mgr->_shouldStop) break;
        }
        xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(10)); 
        xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(10)); 
        
        vTaskDelay(10); 
    }
    
    RmtBlock end; end.itemCount = 0;
    xQueueSend(mgr->_rmtQueue, &end, pdMS_TO_TICKS(100));
    
    mgr->_producerTaskHandle = nullptr; 
    vTaskDelete(NULL);
}

void SubGhzManager::startBruteForce() {
    stop();
    _isBruteForcing = true;
    _shouldStop = false;
    xTaskCreatePinnedToCore(bruteForceTask, "BruteForce", Config::SUBGHZ_STACK_SIZE, this, 1, &_producerTaskHandle, 1);
}

// --- PRODUCER TASK ---
void SubGhzManager::producerTask(void* param) {
    SubGhzManager* mgr = (SubGhzManager*)param;
    float freq = 433.92;
    float deviation = 0.0;
    Modulation mod = Modulation::OOK;
    long dataStart = 0;

    {
        SubGhzLock lock;
        if (!lock.locked()) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }
        File file = SD.open(g_playbackFilePath);
        if (!file) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }

        char line[128];
        while(file.available()) {
            size_t len = file.readBytesUntil('\n', line, 127);
            line[len] = 0;
            if (strncmp(line, "Frequency:", 10) == 0) freq = (float)atol(line + 10) / 1000000.0;
            if (strncmp(line, "Preset:", 7) == 0) {
                if (strstr(line, "FSK")) { mod = Modulation::FSK2; deviation = Config::FSK_DEVIATION_DEFAULT; } 
                else { mod = Modulation::OOK; }
            }
            if (strncmp(line, "RAW_Data:", 9) == 0) { dataStart = file.position(); break; }
        }
        
        mgr->_radio->setFrequency(freq);
        mgr->setModulation(mod, deviation);
        mgr->_radio->transmitDirect(); 
        file.close(); 
    }

    File file = SD.open(g_playbackFilePath);
    file.seek(dataStart);
    RmtBlock block; block.itemCount = 0;
    char line[128]; bool inData = false;

    while (true) {
        if (mgr->_shouldStop) break;
        esp_task_wdt_reset();

        size_t len = 0;
        {
            SubGhzLock lock;
            if (!lock.locked()) { vTaskDelay(5); continue; }
            if (!file.available()) break;
            len = file.readBytesUntil('\n', line, 127);
        }
        
        line[len] = 0;
        if (strncmp(line, "RAW_Data:", 9) == 0) inData = true;
        
        if (inData) {
            char* ptr = line;
            if (strncmp(line, "RAW_Data:", 9) == 0) ptr += 9;
            char* token = strtok(ptr, " ");
            while (token != NULL) {
                int32_t val = atoi(token);
                if (val != 0) {
                    uint32_t duration = abs(val);
                    uint8_t level = (val > 0) ? 1 : 0;
                    while (duration > 32767) {
                        block.items[block.itemCount++] = {{{32767, level, 32767, level}}}; 
                        duration -= 65534;
                        if (duration > 65534) duration = 0;
                        if(block.itemCount >= 64) { 
                            if (xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(50)) != pdTRUE) {
                                if (mgr->_shouldStop) break;
                            }
                            block.itemCount = 0; 
                        }
                        if (duration == 0) break;
                    }
                    if (duration > 0) block.items[block.itemCount++] = {{{ (uint16_t)duration, (uint16_t)level, 0, 0 }}};
                    if(block.itemCount >= 64) { 
                        if (xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(50)) != pdTRUE) {
                             if (mgr->_shouldStop) break;
                        }
                        block.itemCount = 0; 
                    }
                }
                token = strtok(NULL, " ");
            }
        }
        vTaskDelay(1);
    }
    
    if (block.itemCount > 0) xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(100));
    block.itemCount = 0; xQueueSend(mgr->_rmtQueue, &block, pdMS_TO_TICKS(100));
    
    { SubGhzLock lock; file.close(); }
    mgr->_producerTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void SubGhzManager::playFlipperFile(const char* path) {
    if (!SD.exists(path)) return;
    stop(); _isReplaying = true;
    strncpy(g_playbackFilePath, path, 63);
    _shouldStop = false;
    xTaskCreatePinnedToCore(producerTask, "SubGhzProd", Config::SUBGHZ_STACK_SIZE, this, 1, &_producerTaskHandle, 1);
}

bool SubGhzManager::loop(StatusMessage& out) {
    if (_isReplaying || _isBruteForcing) {
        out.state = SystemState::ATTACKING_SUBGHZ_TX;
        out.isReplaying = true;
        RmtBlock block;
        if (xQueueReceive(_rmtQueue, &block, 0) == pdTRUE) {
            if (block.itemCount == 0) {
                _isReplaying = false; _isBruteForcing = false;
                stop();
                snprintf(out.logMsg, MAX_LOG_MSG, "TX Done");
            } else {
                rmt_write_items(RMT_TX_CHANNEL, block.items, block.itemCount, true);
                rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
                snprintf(out.logMsg, MAX_LOG_MSG, "TX RMT...");
            }
        }
        return true;
    }
    
    if(_isCapturing) {
        out.state = SystemState::ANALYZING_SUBGHZ_RX;
        if(g_subGhzIndex > 10 && (micros() - g_subGhzLastTime > Config::SIGNAL_TIMEOUT_US)) {
            g_subGhzCaptureDone = true; stop();
            _isRollingCode = analyzeSignal();
            if (g_subGhzIndex > 20) {
                uint32_t hash = 0; for(size_t i=0; i<g_subGhzIndex; i++) hash += g_subGhzBuffer[i];
                ScriptManager::getInstance().notifySignal(hash);
            }
            snprintf(out.logMsg, MAX_LOG_MSG, _isRollingCode ? "ROLLING CODE!" : "Fixed Code OK");
        } else snprintf(out.logMsg, MAX_LOG_MSG, "Rec: %d", g_subGhzIndex);
        out.rollingCodeDetected = _isRollingCode;
        return true;
    }
    
    if(_isAnalyzing) {
        out.state = SystemState::ANALYZING_SUBGHZ_RX;
        SubGhzLock lock;
        if(lock.locked()) {
            _currentFreq += 0.05; if(_currentFreq > 434.28) _currentFreq = 433.0;
            _radio->setFrequency(_currentFreq);
            int idx = (int)((_currentFreq - 433.0) * 100);
            if(idx >= 0 && idx < 128) out.spectrum[idx] = (uint8_t)(_radio->getRSSI() + 100);
            snprintf(out.logMsg, MAX_LOG_MSG, "%.2f MHz", _currentFreq);
        }
        return true;
    }

    if(_isJamming) {
        out.state = SystemState::ATTACKING_SUBGHZ_TX;
        SubGhzLock lock;
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
void SubGhzManager::startCapture() { stop(); _isCapturing=true; g_subGhzIndex=0; g_subGhzCaptureDone=false; SubGhzLock l; if(l.locked()) { _radio->setFrequency(433.92); _radio->setOOK(true); _radio->receiveDirect(); attachInterrupt(Config::PIN_CC_GDO0, isrHandler, CHANGE); }}
void SubGhzManager::startAnalyzer() { stop(); _isAnalyzing=true; _currentFreq=433.0; SubGhzLock l; if(l.locked()) _radio->standby(); }
void SubGhzManager::startJammer() { stop(); _isJamming=true; SubGhzLock l; if(l.locked()) { _radio->setFrequency(433.92); _radio->transmitDirect(0); }}