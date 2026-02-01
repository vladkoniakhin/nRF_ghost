#include "SubGhzManager.h"
#include "System.h"
#include "Config.h"

// Глобальные буферы (volatile для использования в ISR)
volatile uint16_t g_subGhzBuffer[Config::RAW_BUFFER_SIZE];
volatile size_t g_subGhzIndex = 0;
volatile uint32_t g_subGhzLastTime = 0;
volatile bool g_subGhzCaptureDone = false;

// Обработчик прерывания
void IRAM_ATTR SubGhzManager::isrHandler() {
    if (g_subGhzCaptureDone || g_subGhzIndex >= Config::RAW_BUFFER_SIZE) return;
    uint32_t now = micros(); 
    uint32_t d = now - g_subGhzLastTime; 
    g_subGhzLastTime = now;
    // Простой фильтр шума (игнорируем импульсы < 50 мкс и > 100 мс)
    if (d > 50 && d < 100000) g_subGhzBuffer[g_subGhzIndex++] = (uint16_t)d;
}

SubGhzManager& SubGhzManager::getInstance() { 
    static SubGhzManager i; 
    return i; 
}

SubGhzManager::SubGhzManager() : 
    _radio(nullptr), 
    _module(nullptr), 
    _isAnalyzing(false), 
    _isJamming(false), 
    _isCapturing(false), 
    _isReplaying(false), 
    _isRollingCode(false),
    _currentFreq(433.92) 
{}

void SubGhzManager::setup() {
    _module = new Module(Config::PIN_CC_CS, Config::PIN_CC_GDO0, RADIOLIB_NC);
    _radio = new CC1101(_module);
    
    if(xSemaphoreTake(g_spiMutex, 1000)) {
        // Инициализация на 433.92 МГц
        if(_radio->begin(433.92) == RADIOLIB_ERR_NONE) { 
            _radio->setOutputPower(10); 
            _radio->setOOK(true); 
            // Сразу усыпляем для экономии энергии и снижения шумов
            _radio->standby(); 
        }
        xSemaphoreGive(g_spiMutex);
    }
}

void SubGhzManager::stop() {
    _isAnalyzing = false;
    _isJamming = false;
    _isCapturing = false;
    _isReplaying = false;
    
    detachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0));
    
    if(xSemaphoreTake(g_spiMutex, 500)) { 
        if (_radio) _radio->standby(); 
        xSemaphoreGive(g_spiMutex); 
    }
}

bool SubGhzManager::analyzeSignal() {
    if(g_subGhzIndex < 20) return false;
    
    // Простейшая эвристика Rolling Code: считаем количество уникальных длительностей импульсов
    int uniqueCounts = 0; 
    uint16_t uniqueTimings[16] = {0};
    
    for(size_t i = 1; i < g_subGhzIndex; i++) {
        bool found = false; 
        for(int j = 0; j < uniqueCounts; j++) {
            if(abs(g_subGhzBuffer[i] - uniqueTimings[j]) < 150) { 
                found = true; 
                break; 
            }
        }
        if(!found) { 
            if(uniqueCounts < 16) uniqueTimings[uniqueCounts++] = g_subGhzBuffer[i]; 
            else return true; // Слишком много уникальных таймингов -> скорее всего Rolling Code или шум
        }
    }
    return (uniqueCounts > 5);
}

void SubGhzManager::startCapture() {
    stop(); 
    _isCapturing = true; 
    _isRollingCode = false; 
    g_subGhzIndex = 0; 
    g_subGhzCaptureDone = false;
    
    if(xSemaphoreTake(g_spiMutex, 500)) {
        _radio->setFrequency(433.92); 
        _radio->setOOK(true); 
        _radio->receiveDirect();
        
        g_subGhzLastTime = micros();
        attachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0), SubGhzManager::isrHandler, CHANGE);
        
        xSemaphoreGive(g_spiMutex);
    }
}

void SubGhzManager::startReplay() {
    // Воспроизведение из RAM (RAW)
    if(g_subGhzIndex < 10 || _isRollingCode) return;
    
    stop(); 
    _isReplaying = true;
    
    if(xSemaphoreTake(g_spiMutex, portMAX_DELAY)) {
        _radio->setFrequency(433.92); 
        _radio->setOOK(true); 
        _radio->transmitDirect();
        
        pinMode(Config::PIN_CC_GDO0, OUTPUT);
        portDISABLE_INTERRUPTS(); // Критическая секция для точности таймингов
        
        bool level = true; 
        for(size_t i = 1; i < g_subGhzIndex; i++) { 
            digitalWrite(Config::PIN_CC_GDO0, level); 
            delayMicroseconds(g_subGhzBuffer[i]); 
            level = !level; 
        }
        
        digitalWrite(Config::PIN_CC_GDO0, LOW);
        portENABLE_INTERRUPTS();
        
        pinMode(Config::PIN_CC_GDO0, INPUT); 
        _radio->standby();
        xSemaphoreGive(g_spiMutex);
    }
    _isReplaying = false;
}

// --- v3.0: Flipper Zero .sub Parser ---
void SubGhzManager::playFlipperFile(const char* path) {
    if (!SD.exists(path)) return;
    stop(); 
    _isReplaying = true;

    if(xSemaphoreTake(g_spiMutex, portMAX_DELAY)) {
        File file = SD.open(path);
        if (file) {
            // Настройка радио (Хардкод 433.92 для MVP, нужен парсер заголовка файла)
            _radio->setFrequency(433.92); 
            _radio->setOOK(true); 
            _radio->transmitDirect();
            
            pinMode(Config::PIN_CC_GDO0, OUTPUT);

            char line[128];
            bool inData = false;
            
            // Включаем "потоковый" режим: читаем кусок -> отправляем
            // Внимание: чтение с SD внутри критической секции таймингов не идеально,
            // но для v3.0 на одном ядре это компромисс.
            
            while (file.available()) {
                // Читаем строку
                size_t len = file.readBytesUntil('\n', line, 127);
                line[len] = 0;

                // Ищем начало данных
                if (strstr(line, "RAW_Data:")) inData = true;
                
                if (inData) {
                    char* ptr = line;
                    if (strstr(line, "RAW_Data:")) ptr += 9; // Пропускаем префикс
                    
                    char* token = strtok(ptr, " ");
                    
                    // Блокируем прерывания только на время отправки одной строки таймингов
                    portDISABLE_INTERRUPTS();
                    while (token != NULL) {
                        int32_t val = atoi(token);
                        // Flipper формат: положительное число = HIGH, отрицательное = LOW
                        if (val > 0) {
                            digitalWrite(Config::PIN_CC_GDO0, HIGH);
                            delayMicroseconds(val);
                        } else {
                            digitalWrite(Config::PIN_CC_GDO0, LOW);
                            delayMicroseconds(-val);
                        }
                        token = strtok(NULL, " ");
                    }
                    portENABLE_INTERRUPTS();
                }
            }
            
            digitalWrite(Config::PIN_CC_GDO0, LOW);
            pinMode(Config::PIN_CC_GDO0, INPUT);
            _radio->standby();
            file.close();
        }
        xSemaphoreGive(g_spiMutex);
    }
    _isReplaying = false;
}

void SubGhzManager::startAnalyzer() { 
    stop(); 
    _isAnalyzing = true; 
    _currentFreq = 433.0; 
    
    if(xSemaphoreTake(g_spiMutex, 500)) {
        if(_radio) _radio->standby(); 
        xSemaphoreGive(g_spiMutex);
    } 
}

void SubGhzManager::startJammer() { 
    stop(); 
    _isJamming = true; 
    
    if(xSemaphoreTake(g_spiMutex, 500)) {
        _radio->setFrequency(433.92); 
        _radio->transmitDirect(0); // Постоянная несущая
        xSemaphoreGive(g_spiMutex);
    } 
}

bool SubGhzManager::loop(StatusMessage& out) {
    if(_isCapturing) {
        out.state = SystemState::ANALYZING_SUBGHZ;
        // Проверка таймаута (конец сигнала)
        if(g_subGhzIndex > 10 && (micros() - g_subGhzLastTime > Config::SIGNAL_TIMEOUT_US)) {
            g_subGhzCaptureDone = true; 
            stop();
            _isRollingCode = analyzeSignal();
            snprintf(out.logMsg, MAX_LOG_MSG, _isRollingCode ? "ROLLING CODE!" : "Fixed Code OK");
        } else {
            snprintf(out.logMsg, MAX_LOG_MSG, "Rec: %d", g_subGhzIndex);
        }
        out.rollingCodeDetected = _isRollingCode;
        return true;
    }
    
    if(_isReplaying) { 
        out.state = SystemState::ATTACKING_SUBGHZ; 
        out.isReplaying = true; 
        return true; 
    }
    
    if(_isAnalyzing) {
        out.state = SystemState::ANALYZING_SUBGHZ;
        if(xSemaphoreTake(g_spiMutex, 20)) {
            _currentFreq += 0.05; 
            if(_currentFreq > 434.28) _currentFreq = 433.0;
            
            _radio->setFrequency(_currentFreq);
            int idx = (int)((_currentFreq - 433.0) * 100);
            
            if(idx >= 0 && idx < 128) {
                out.spectrum[idx] = (uint8_t)(_radio->getRSSI() + 100);
            }
            snprintf(out.logMsg, MAX_LOG_MSG, "%.2f MHz", _currentFreq);
            xSemaphoreGive(g_spiMutex);
        }
        return true;
    }
    
    if(_isJamming) {
        out.state = SystemState::ATTACKING_SUBGHZ;
        if(xSemaphoreTake(g_spiMutex, 5)) { 
            _radio->transmitDirect(0); 
            xSemaphoreGive(g_spiMutex); 
        }
        snprintf(out.logMsg, MAX_LOG_MSG, "Jamming"); 
        vTaskDelay(100);
        return true;
    }
    
    return false;
}