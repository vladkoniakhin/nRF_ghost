#include "SdManager.h"
#include "System.h"

SdManager& SdManager::getInstance() { static SdManager i; return i; }
SdManager::SdManager() : _isMounted(false), _isCapturing(false), _fileIndex(0) { _packetQueue = xQueueCreate(Config::PCAP_QUEUE_SIZE, sizeof(CapturedPacket)); }

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
    if(!_isMounted||!_isCapturing) return false;
    CapturedPacket p; p.timestamp=millis(); p.length=(l>256)?256:l; memcpy(p.data,b,p.length);
    BaseType_t w=pdFALSE; xQueueSendFromISR(_packetQueue, &p, &w); return w==pdTRUE;
}

void SdManager::writeTask(void* p) {
    SdManager* s=(SdManager*)p; CapturedPacket k;
    for(;;) {
        if(xQueueReceive(s->_packetQueue, &k, portMAX_DELAY)) {
            if(s->_pcapFile && xSemaphoreTake(g_spiMutex, 5)) {
                PcapPacketHeader h; h.ts_sec=k.timestamp/1000; h.ts_usec=(k.timestamp%1000)*1000; h.incl_len=k.length; h.orig_len=k.length;
                s->_pcapFile.write((uint8_t*)&h,sizeof(h)); s->_pcapFile.write(k.data,k.length);
                xSemaphoreGive(g_spiMutex);
            }
        }
    }
}