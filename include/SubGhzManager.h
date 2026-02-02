#pragma once
#include "Common.h"
#include "Engines.h"
#include <RadioLib.h>
#include <driver/rmt.h>

// Блок данных для очереди RMT
struct RmtBlock {
    size_t itemCount;
    rmt_item32_t items[64];
};

// Типы модуляции для v5.0
enum class Modulation { OOK, FSK2 };

class SubGhzManager : public IAttackEngine {
public:
    static SubGhzManager& getInstance();
    
    // IAttackEngine методы
    void setup() override;
    bool loop(StatusMessage& statusOut) override;
    void stop() override;
    
    // Управление режимами
    void startAnalyzer();
    void startJammer();
    void startCapture();
    
    // v5.0: RMT Streaming
    void playFlipperFile(const char* path);
    
    // --- FIX: Добавлен геттер, которого не хватало ---
    bool isReplaying() const; 

private:
    SubGhzManager();
    
    CC1101* _radio;
    Module* _module;
    
    // Состояния
    bool _isAnalyzing;
    bool _isJamming;
    bool _isCapturing;
    bool _isReplaying;
    bool _isRollingCode;
    float _currentFreq;
    Modulation _currentModulation;
    
    // RMT
    QueueHandle_t _rmtQueue;
    TaskHandle_t _producerTaskHandle;
    
    static void producerTask(void* param);
    void configureRmt();
    void setModulation(Modulation mod, float dev);
    
    bool analyzeSignal();
    static void IRAM_ATTR isrHandler();
};