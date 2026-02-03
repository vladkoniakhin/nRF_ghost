#include "SubGhzManager.h"
#include "System.h"
#include "Config.h"
#include "ScriptManager.h"
#include <esp_task_wdt.h>
#include <FS.h>
#include <SD.h>
#include <driver/rmt.h> // RMT Driver

// --- HARDWARE CONFIGURATION ---
// Используем RMT канал 0, GPIO 15
#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV 80  // 80MHz / 80 = 1MHz (1 tick = 1 us)

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

// --- INTERRUPT HANDLER (RX) ---
void IRAM_ATTR SubGhzManager::isrHandler() {
    if (g_subGhzCaptureDone || g_subGhzIndex >= Config::RAW_BUFFER_SIZE) return;
    uint32_t now = micros(); 
    uint32_t d = now - g_subGhzLastTime; 
    g_subGhzLastTime = now;
    // Фильтр шума < 50мкс
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

// --- RMT CONFIGURATION (Hardware Abstraction) ---
void SubGhzManager::configureRmt() {
    rmt_config_t config;
    config.rmt_mode = RMT_MODE_TX;
    config.channel = RMT_TX_CHANNEL;
    config.gpio_num = (gpio_num_t)Config::PIN_CC_GDO0;
    config.mem_block_num = 1;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false; // Нет несущей, модулирует сам CC1101
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    config.clk_div = RMT_CLK_DIV; // 1us resolution

    rmt_config(&config);
    rmt_driver_install(config.channel, 0, 0);
}

void SubGhzManager::setup() {
    _module = new Module(Config::PIN_CC_CS, Config::PIN_CC_GDO0, RADIOLIB_NC);
    _radio = new CC1101(_module);
    
    SubGhzLock lock;
    if(lock.locked()) { 
        // Инициализация CC1101
        if(_radio->begin(433.92) == RADIOLIB_ERR_NONE) { 
            _radio->setOutputPower(10); 
            _radio->setOOK(true); 
            _radio->standby(); 
        } 
    }
    
    // Настраиваем RMT драйвер на пин GDO0 (в режиме TX он работает как вход модуляции)
    configureRmt();
}

void SubGhzManager::stop() {
    bool wasCapturing = _isCapturing;
    _isAnalyzing = false; _isJamming = false; 
    _isCapturing = false; _isReplaying = false; _isBruteForcing = false;
    
    detachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0));
    
    // Остановка задачи генератора
    if (_producerTaskHandle != nullptr) {
        _shouldStop = true;
        uint32_t start = millis();
        while (_producerTaskHandle != nullptr && millis() - start < 1000) vTaskDelay(10);
        if (_producerTaskHandle != nullptr) { vTaskDelete(_producerTaskHandle); _producerTaskHandle = nullptr; }
    }
    _shouldStop = false; 
    xQueueReset(_rmtQueue);
    
    // Перевод радио в сон
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

// --- RMT DRIVEN TASKS ---

// Helper for file reading
size_t readLineSafely(File& f, char* buf, size_t maxLen) {
    size_t count = 0;
    while (f.available() && count < maxLen - 1) {
        char c = f.read();
        if (c == '\n') break;
        if (c != '\r') buf[count++] = c;
    }
    buf[count] = 0; return count;
}

void SubGhzManager::bruteForceTask(void* param) {
    SubGhzManager* mgr = (SubGhzManager*)param;
    
    // Настройка радио на передачу
    { 
        SubGhzLock lock; 
        if (!lock.locked()) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }
        mgr->_radio->setFrequency(433.92); 
        mgr->_radio->setOOK(true); 
        // Включаем передачу, модуляция пойдет через GDO0 от RMT
        mgr->_radio->transmitDirect(); 
    }

    uint16_t Te = Config::CAME_BIT_PERIOD;
    // RMT Item: { duration, level, duration, level }
    // Мы можем паковать по 2 импульса в один элемент
    
    // Буфер для одного кода (12 бит + пилот + пауза)
    // Максимум 32 элемента (хватит на CAME)
    rmt_item32_t codeBuffer[32]; 

    for (uint16_t code = 0; code < 4096; code++) {
        if (mgr->_shouldStop) break;
        esp_task_wdt_reset();
        
        int itemIdx = 0;
        
        // Start bit / Pilot (CAME specific)
        codeBuffer[itemIdx++] = {{{ (uint16_t)Te, 1, (uint16_t)(36*Te), 0 }}}; 

        for (int b = 11; b >= 0; b--) {
            bool bit = (code >> b) & 1;
            // CAME coding: 
            // 0 = 1 Te HIGH, 2 Te LOW
            // 1 = 2 Te HIGH, 1 Te LOW
            if (bit == 0) {
                codeBuffer[itemIdx++] = {{{ (uint16_t)Te, 1, (uint16_t)(2*Te), 0 }}};
            } else {
                codeBuffer[itemIdx++] = {{{ (uint16_t)(2*Te), 1, (uint16_t)Te, 0 }}};
            }
        }
        
        // Inter-code pause
        codeBuffer[itemIdx++] = {{{ 0, 0, (uint16_t)(36*Te), 0 }}};
        
        // ОТПРАВКА ЧЕРЕЗ RMT (Блокирующая, но аппаратная)
        rmt_write_items(RMT_TX_CHANNEL, codeBuffer, itemIdx, true);
        rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
        
        // Маленькая пауза между кодами, чтобы не забить эфир сплошняком
        vTaskDelay(2); 
    }
    
    mgr->_producerTaskHandle = nullptr; 
    vTaskDelete(NULL);
}

void SubGhzManager::producerTask(void* param) {
    SubGhzManager* mgr = (SubGhzManager*)param;
    float freq = 433.92; float deviation = 0.0; Modulation mod = Modulation::OOK; long dataStart = 0;
    char line[128]; 

    {
        SubGhzLock lock;
        if (!lock.locked()) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }
        File file = SD.open(g_playbackFilePath);
        if (!file) { mgr->_producerTaskHandle = nullptr; vTaskDelete(NULL); return; }

        while(file.available()) {
            readLineSafely(file, line, 128);
            if (strncmp(line, "Frequency:", 10) == 0) freq = (float)atol(line + 10) / 1000000.0;
            if (strncmp(line, "Preset:", 7) == 0) { if (strstr(line, "FSK")) { mod = Modulation::FSK2; deviation = Config::FSK_DEVIATION_DEFAULT; } else { mod = Modulation::OOK; } }
            if (strncmp(line, "RAW_Data:", 9) == 0) { dataStart = file.position(); break; }
        }
        mgr->_radio->setFrequency(freq); mgr->setModulation(mod, deviation); 
        mgr->_radio->transmitDirect(); // Radio is ON, waiting for RMT pulses
        file.close(); 
    }

    File file = SD.open(g_playbackFilePath); 
    file.seek(dataStart);
    
    // RMT Buffer Management
    const int RMT_CHUNK_SIZE = 64;
    rmt_item32_t rmtBuffer[RMT_CHUNK_SIZE];
    int rmtIdx = 0;

    while (true) {
        if (mgr->_shouldStop) break; 
        esp_task_wdt_reset();
        
        {
            SubGhzLock lock; 
            if (!lock.locked()) { vTaskDelay(5); continue; }
            if (!file.available()) break;
            readLineSafely(file, line, 128);
        }
        
        bool inData = (strncmp(line, "RAW_Data:", 9) == 0);
        if (inData) {
            char* ptr = line + 9;
            char* token = strtok(ptr, " ");
            while (token != NULL) {
                int32_t val = atoi(token);
                if (val != 0) {
                    uint32_t duration = abs(val); 
                    uint8_t level = (val > 0) ? 1 : 0;
                    
                    // RMT duration limit check (15 bit = 32767)
                    while (duration > 32767) {
                        rmtBuffer[rmtIdx++] = {{{32767, level, 32767, level}}}; 
                        duration -= 65534;
                        if (rmtIdx >= RMT_CHUNK_SIZE) {
                            rmt_write_items(RMT_TX_CHANNEL, rmtBuffer, rmtIdx, true);
                            rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
                            rmtIdx = 0;
                        }
                        if (duration == 0) break;
                    }
                    
                    if (duration > 0) {
                        // Используем одну половину rmt_item, вторая 0 (будет проигнорирована следующим, если правильно сложить, или просто займет время)
                        // Для OOK лучше формировать пары High-Low. Здесь упрощенно: каждый сэмпл - отдельный полу-айтем.
                        // Hack: RMT ожидает пары. Если у нас поток RAW данных, мы должны их парить.
                        // Для надежности v6.4 просто шлем duration + уровень, а второй слот 0 длительности (пустышка)
                        rmtBuffer[rmtIdx++] = {{{ (uint16_t)duration, level, 0, 0 }}}; 
                    }
                    
                    if (rmtIdx >= RMT_CHUNK_SIZE) {
                        // SEND TO HARDWARE
                        rmt_write_items(RMT_TX_CHANNEL, rmtBuffer, rmtIdx, true);
                        rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
                        rmtIdx = 0;
                    }
                }
                token = strtok(NULL, " ");
            }
        }
        // Небольшая задержка, чтобы не вешать SD шину
        vTaskDelay(1); 
    }
    
    // Send remaining items
    if (rmtIdx > 0) {
        rmt_write_items(RMT_TX_CHANNEL, rmtBuffer, rmtIdx, true);
        rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
    }

    { SubGhzLock lock; file.close(); }
    mgr->_producerTaskHandle = nullptr; 
    vTaskDelete(NULL);
}

void SubGhzManager::playFlipperFile(const char* path) {
    if (!SD.exists(path)) return; stop(); _isReplaying = true;
    strncpy(g_playbackFilePath, path, 63); _shouldStop = false;
    xTaskCreatePinnedToCore(producerTask, "SubGhzProd", Config::SUBGHZ_STACK_SIZE, this, 1, &_producerTaskHandle, 1);
}

void SubGhzManager::startBruteForce() {
    stop(); _isBruteForcing = true; _shouldStop = false;
    xTaskCreatePinnedToCore(bruteForceTask, "BruteForce", Config::SUBGHZ_STACK_SIZE, this, 1, &_producerTaskHandle, 1);
}

void SubGhzManager::startCapture() { stop(); _isCapturing=true; g_subGhzIndex=0; g_subGhzCaptureDone=false; SubGhzLock l; if(l.locked()) { _radio->setFrequency(433.92); _radio->setOOK(true); _radio->receiveDirect(); attachInterrupt(Config::PIN_CC_GDO0, isrHandler, CHANGE); }}
void SubGhzManager::startAnalyzer() { stop(); _isAnalyzing=true; _currentFreq=433.0; SubGhzLock l; if(l.locked()) _radio->standby(); }
void SubGhzManager::startJammer() { stop(); _isJamming=true; SubGhzLock l; if(l.locked()) { _radio->setFrequency(433.92); _radio->transmitDirect(0); }}

// Loop simplified for RMT status
bool SubGhzManager::loop(StatusMessage& out) {
    if (_isReplaying || _isBruteForcing) {
        out.state = SystemState::ATTACKING_SUBGHZ_TX; 
        out.isReplaying = true;
        if (_producerTaskHandle == nullptr) { // Task finished
             _isReplaying = false; _isBruteForcing = false; 
             stop(); 
             snprintf(out.logMsg, MAX_LOG_MSG, "TX Done"); 
        } else {
             snprintf(out.logMsg, MAX_LOG_MSG, "TX (Hardware RMT)..."); 
        }
        return true;
    }
    // ... capture and analysis logic same as before ...
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