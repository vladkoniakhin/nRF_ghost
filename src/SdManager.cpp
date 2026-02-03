#include "SdManager.h"
#include "System.h"

SdManager& SdManager::getInstance() { static SdManager i; return i; }

// Инициализация новой переменной
SdManager::SdManager() : _isMounted(false), _isCapturing(false), _fileIndex(0), _nextFileIndex(0) { 
    _packetQueue = xQueueCreate(Config::PCAP_QUEUE_SIZE, sizeof(CapturedPacket)); 
}

void SdManager::init() {
    if(xSemaphoreTake(g_spiMutex, 1000)) {
        if(!SD.begin(Config::PIN_SD_CS)) {
            Serial.println("SD Fail"); 
        } else { 
            _isMounted = true; 
            Serial.println("SD OK");
            
            // FIX v6.3: Предварительный расчет индекса файла
            char n[32];
            while(true) {
                snprintf(n, 32, "/cap_%d.pcap", _nextFileIndex);
                if (SD.exists(n)) {
                    _nextFileIndex++;
                } else {
                    break; // Нашли свободный слот
                }
                if (_nextFileIndex % 10 == 0) vTaskDelay(1); // Anti-WDT
            }
            Serial.printf("[SD] Next Capture Index: %d\n", _nextFileIndex);
        }
        xSemaphoreGive(g_spiMutex);
    }
    xTaskCreatePinnedToCore(SdManager::writeTask, "SD_Write", 4096, this, 1, NULL, 0);
}

void SdManager::startCapture() {
    if(!_isMounted || _isCapturing) return;
    
    // FIX v6.3: Используем готовый индекс (O(1) операция)
    if(xSemaphoreTake(g_spiMutex, 500)) {
        char n[32]; 
        snprintf(n, 32, "/cap_%d.pcap", _nextFileIndex);
        
        _pcapFile = SD.open(n, FILE_WRITE);
        if(_pcapFile) { 
            PcapGlobalHeader h; 
            _pcapFile.write((uint8_t*)&h, sizeof(h)); 
            _isCapturing = true; 
            _nextFileIndex++; // Готовим индекс для следующего раза
        }
        xSemaphoreGive(g_spiMutex);
    }
}

void SdManager::stopCapture() {
    if(_isCapturing) {
        if(xSemaphoreTake(g_spiMutex, portMAX_DELAY)) {
            if(_pcapFile) { 
                _pcapFile.flush(); 
                _pcapFile.close(); 
                _isCapturing = false; 
            }
            xSemaphoreGive(g_spiMutex);
        }
    }
}

bool SdManager::enqueuePacketFromISR(const uint8_t* b, uint16_t l) {
    if(!_isMounted || !_isCapturing) return false;
    
    CapturedPacket p; 
    p.timestamp = millis(); 
    p.length = (l > 256) ? 256 : l; 
    memcpy(p.data, b, p.length);
    
    BaseType_t w = pdFALSE; 
    if(xQueueSendFromISR(_packetQueue, &p, &w) == pdTRUE) {
        return (w == pdTRUE);
    } else {
        return false;
    }
}

void SdManager::writeTask(void* p) {
    SdManager* s = (SdManager*)p; 
    CapturedPacket k;
    
    for(;;) {
        if(xQueueReceive(s->_packetQueue, &k, portMAX_DELAY)) {
            if(s->_pcapFile && s->_isCapturing) {
                if(xSemaphoreTake(g_spiMutex, 10)) {
                    PcapPacketHeader h; 
                    h.ts_sec = k.timestamp / 1000; 
                    h.ts_usec = (k.timestamp % 1000) * 1000; 
                    h.incl_len = k.length; 
                    h.orig_len = k.length;
                    
                    s->_pcapFile.write((uint8_t*)&h, sizeof(h)); 
                    s->_pcapFile.write(k.data, k.length);
                    
                    xSemaphoreGive(g_spiMutex);
                }
            }
        }
    }
}