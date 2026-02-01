#include "SubGhzManager.h"
#include "System.h"
#include "Config.h"

volatile uint16_t g_subGhzBuffer[Config::RAW_BUFFER_SIZE];
volatile size_t g_subGhzIndex = 0;
volatile uint32_t g_subGhzLastTime = 0;
volatile bool g_subGhzCaptureDone = false;

void IRAM_ATTR SubGhzManager::isrHandler() {
    if (g_subGhzCaptureDone || g_subGhzIndex >= Config::RAW_BUFFER_SIZE) return;
    uint32_t now = micros(); uint32_t d = now - g_subGhzLastTime; g_subGhzLastTime = now;
    if (d > 50 && d < 100000) g_subGhzBuffer[g_subGhzIndex++] = (uint16_t)d;
}

SubGhzManager& SubGhzManager::getInstance() { static SubGhzManager i; return i; }
SubGhzManager::SubGhzManager() : _radio(nullptr), _module(nullptr), _isAnalyzing(false), _isJamming(false), _isCapturing(false), _isReplaying(false), _currentFreq(433.92), _isRollingCode(false) {}

void SubGhzManager::setup() {
    _module = new Module(Config::PIN_CC_CS, Config::PIN_CC_GDO0, RADIOLIB_NC);
    _radio = new CC1101(_module);
    if(xSemaphoreTake(g_spiMutex, 1000)) {
        if(_radio->begin(433.92) == RADIOLIB_ERR_NONE) { _radio->setOutputPower(10); _radio->setOOK(true); }
        xSemaphoreGive(g_spiMutex);
    }
}

void SubGhzManager::stop() {
    _isAnalyzing=_isJamming=_isCapturing=_isReplaying=false;
    detachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0));
    if(xSemaphoreTake(g_spiMutex, 500)) { _radio->standby(); xSemaphoreGive(g_spiMutex); }
}

bool SubGhzManager::analyzeSignal() {
    if(g_subGhzIndex<20) return false;
    int u=0; uint16_t k[16]={0};
    for(size_t i=1;i<g_subGhzIndex;i++) {
        bool m=false; for(int j=0;j<u;j++) if(abs(g_subGhzBuffer[i]-k[j])<150) { m=true; break; }
        if(!m) { if(u<16) k[u++]=g_subGhzBuffer[i]; else return true; }
    }
    return (u > 5);
}

void SubGhzManager::startCapture() {
    stop(); _isCapturing=true; _isRollingCode=false; g_subGhzIndex=0; g_subGhzCaptureDone=false;
    if(xSemaphoreTake(g_spiMutex, 500)) {
        _radio->setFrequency(433.92); _radio->setOOK(true); _radio->receiveDirect();
        g_subGhzLastTime=micros();
        attachInterrupt(digitalPinToInterrupt(Config::PIN_CC_GDO0), SubGhzManager::isrHandler, CHANGE);
        xSemaphoreGive(g_spiMutex);
    }
}

void SubGhzManager::startReplay() {
    if(g_subGhzIndex<10 || _isRollingCode) return;
    stop(); _isReplaying=true;
    if(xSemaphoreTake(g_spiMutex, portMAX_DELAY)) {
        _radio->setFrequency(433.92); _radio->setOOK(true); _radio->transmitDirect();
        pinMode(Config::PIN_CC_GDO0, OUTPUT);
        portDISABLE_INTERRUPTS();
        bool l=true; for(size_t i=1;i<g_subGhzIndex;i++) { digitalWrite(Config::PIN_CC_GDO0, l); delayMicroseconds(g_subGhzBuffer[i]); l=!l; }
        digitalWrite(Config::PIN_CC_GDO0, LOW);
        portENABLE_INTERRUPTS();
        pinMode(Config::PIN_CC_GDO0, INPUT); _radio->standby();
        xSemaphoreGive(g_spiMutex);
    }
    _isReplaying=false;
}

void SubGhzManager::startAnalyzer() { stop(); _isAnalyzing=true; _currentFreq=433.0; if(xSemaphoreTake(g_spiMutex,500)){_radio->standby(); xSemaphoreGive(g_spiMutex);} }
void SubGhzManager::startJammer() { stop(); _isJamming=true; if(xSemaphoreTake(g_spiMutex,500)){_radio->setFrequency(433.92); _radio->transmitDirect(0); xSemaphoreGive(g_spiMutex);} }

bool SubGhzManager::loop(StatusMessage& out) {
    if(_isCapturing) {
        out.state=SystemState::ANALYZING_SUBGHZ;
        if(g_subGhzIndex>10 && (micros()-g_subGhzLastTime > Config::SIGNAL_TIMEOUT_US)) {
            g_subGhzCaptureDone=true; stop();
            _isRollingCode = analyzeSignal();
            snprintf(out.logMsg, MAX_LOG_MSG, _isRollingCode?"ROLLING CODE!":"Fixed Code OK");
        } else snprintf(out.logMsg, MAX_LOG_MSG, "Rec: %d", g_subGhzIndex);
        out.rollingCodeDetected = _isRollingCode;
        return true;
    }
    if(_isReplaying) { out.state=SystemState::ATTACKING_SUBGHZ; out.isReplaying=true; return true; }
    if(_isAnalyzing) {
        out.state=SystemState::ANALYZING_SUBGHZ;
        if(xSemaphoreTake(g_spiMutex, 20)) {
            _currentFreq+=0.05; if(_currentFreq>434.28) _currentFreq=433.0;
            _radio->setFrequency(_currentFreq);
            int idx=(int)((_currentFreq-433.0)*100);
            if(idx>=0&&idx<128) out.spectrum[idx]=(uint8_t)(_radio->getRSSI()+100);
            snprintf(out.logMsg, MAX_LOG_MSG, "%.2f MHz", _currentFreq);
            xSemaphoreGive(g_spiMutex);
        }
        return true;
    }
    if(_isJamming) {
        out.state=SystemState::ATTACKING_SUBGHZ;
        if(xSemaphoreTake(g_spiMutex, 5)){ _radio->transmitDirect(0); xSemaphoreGive(g_spiMutex); }
        snprintf(out.logMsg, MAX_LOG_MSG, "Jamming"); vTaskDelay(100);
        return true;
    }
    return false;
}