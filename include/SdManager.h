#pragma once
#include "Common.h"
#include "Config.h"
#include <SD.h>
#include <SPI.h>

struct PcapGlobalHeader {
    uint32_t magic_number   = 0xa1b2c3d4;
    uint16_t version_major  = 2;
    uint16_t version_minor  = 4;
    int32_t  thiszone       = 0;
    uint32_t sigfigs        = 0;
    uint32_t snaplen        = 65535;
    uint32_t network        = 105; // DLT_IEEE802_11
};

struct PcapPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};

class SdManager {
public:
    static SdManager& getInstance();
    void init();
    void startCapture();
    void stopCapture();
    bool enqueuePacket(const uint8_t* buf, uint16_t len);
    bool enqueuePacketFromISR(const uint8_t* buf, uint16_t len);
    bool isMounted() const { return _isMounted; }
    bool isCapturing() const { return _isCapturing; }

private:
    SdManager();
    SdManager(const SdManager&) = delete;
    void operator=(const SdManager&) = delete;
    static void writeTask(void* parameter);
    bool _isMounted;
    bool _isCapturing;
    File _pcapFile;
    QueueHandle_t _packetQueue;
    uint32_t _fileIndex;
};