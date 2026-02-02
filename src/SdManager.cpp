#include "SdManager.h"
#include "System.h"

SdManager& SdManager::getInstance() { static SdManager i; return i; }
SdManager::SdManager() : _isMounted(false), _isCapturing(false), _fileIndex(0) { 
    _packetQueue = xQueueCreate(Config::PCAP_QUEUE_SIZE, sizeof(CapturedPacket)); 
}

void SdManager::init() {
    if(xSemaphoreTake(g_spiMutex, 1000)) {
        if(!SD.begin(Config::PIN_SD_CS)) Serial.println("SD Fail"); else { _isMounted=true; Serial.println("SD OK"); }
        xSemaphoreGive(g_spiMutex);
    }
    xTaskCreatePinnedToCore(SdManager::writeTask, "SD_Write", 4096, this, 1, NULL, 0);
}

void SdManager::startCapture() {
    if(!_isMounted || _isCapturing) return;
    if(xSemaphoreTake(g_spiMutex, 500)) {
        char n[32]; do { snprintf(n,32,"/cap_%d.pcap",_fileIndex++); } while(SD.exists(n));
        _pcapFile = SD.open(n, FILE_WRITE);
        if(_pcapFile) { PcapGlobalHeader h; _pcapFile.write((uint8_t*)&h,sizeof(h)); _isCapturing=true; }
        xSemaphoreGive(g_spiMutex);
    }
}

void SdManager::stopCapture() {
    if(_isCapturing) {
        if(xSemaphoreTake(g_spiMutex, portMAX_DELAY)) {
            if(_pcapFile) { _pcapFile.flush(); _pcapFile.close(); _isCapturing=false; }
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
    // xQueueSendFromISR не блокирует прерывание. 
    // Если очередь полна, пакет просто дропается.
    if(xQueueSendFromISR(_packetQueue, &p, &w) == pdTRUE) {
        return (w == pdTRUE);
    } else {
        // Queue full -> Drop packet silently to save CPU
        return false;
    }
}

void SdManager::writeTask(void* p) {
    SdManager* s = (SdManager*)p; 
    CapturedPacket k;
    
    for(;;) {
        // Ждем пакет из очереди
        if(xQueueReceive(s->_packetQueue, &k, portMAX_DELAY)) {
            if(s->_pcapFile && s->_isCapturing) {
                // Пытаемся взять SPI мьютекс. Если занят (экран рисует), ждем немного.
                if(xSemaphoreTake(g_spiMutex, 10)) {
                    PcapPacketHeader h; 
                    h.ts_sec = k.timestamp / 1000; 
                    h.ts_usec = (k.timestamp % 1000) * 1000; 
                    h.incl_len = k.length; 
                    h.orig_len = k.length;
                    
                    s->_pcapFile.write((uint8_t*)&h, sizeof(h)); 
                    s->_pcapFile.write(k.data, k.length);
                    
                    // Flush не делаем каждый раз для скорости, система сама сбросит буфер
                    xSemaphoreGive(g_spiMutex);
                } else {
                    // Если мьютекс занят долго, данные теряются? 
                    // Нет, мы просто не записали их в файл сейчас, но они уже извлечены из очереди.
                    // Придется выкинуть этот пакет, чтобы не тормозить.
                    // В реальном RTOS мы бы вернули его в очередь, но здесь проще дропнуть.
                }
            }
        }
    }
}