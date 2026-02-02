#pragma once
#include "Common.h"
#include "Engines.h"
#include <RadioLib.h>
#include <driver/rmt.h>

struct RmtBlock {
    size_t itemCount;
    rmt_item32_t items[64];
};

enum class Modulation { OOK, FSK2 };

class SubGhzManager : public IAttackEngine {
public:
    static SubGhzManager& getInstance();
    
    void setup() override;
    bool loop(StatusMessage& statusOut) override;
    void stop() override;
    
    void startAnalyzer();
    void startJammer();
    void startCapture();
    
    // Новая атака: Перебор кодов
    void startBruteForce(); 
    
    void playFlipperFile(const char* path);
    bool isReplaying() const; 

private:
    SubGhzManager();
    
    CC1101* _radio;
    Module* _module;
    
    bool _isAnalyzing;
    bool _isJamming;
    bool _isCapturing;
    bool _isReplaying;
    bool _isBruteForcing;
    bool _isRollingCode;
    float _currentFreq;
    Modulation _currentModulation;
    
    QueueHandle_t _rmtQueue;
    TaskHandle_t _producerTaskHandle;
    
    static void producerTask(void* param);
    static void bruteForceTask(void* param);
    
    void configureRmt();
    void setModulation(Modulation mod, float dev);
    bool analyzeSignal();
    static void IRAM_ATTR isrHandler();
};