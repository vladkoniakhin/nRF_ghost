#pragma once
#include "Common.h"
#include "Engines.h"
#include <RadioLib.h>

class SubGhzManager : public IAttackEngine {
public:
    static SubGhzManager& getInstance();
    void setup() override;
    bool loop(StatusMessage& statusOut) override;
    void stop() override;
    void startAnalyzer();
    void startJammer();
    void startCapture();
    void startReplay();

private:
    SubGhzManager();
    CC1101* _radio;
    Module* _module;
    bool _isAnalyzing;
    bool _isJamming;
    bool _isCapturing;
    bool _isReplaying;
    bool _isRollingCode;
    float _currentFreq;
    bool analyzeSignal();
    static void IRAM_ATTR isrHandler();
};