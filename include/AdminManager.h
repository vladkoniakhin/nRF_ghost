#pragma once
#include "WebPortalManager.h"
// Wrapper class to fit old architecture calls
class AdminManager {
public:
    static AdminManager& getInstance() { static AdminManager i; return i; }
    void start() { WebPortalManager::getInstance().start("nRF_Admin"); }
    void stop() { WebPortalManager::getInstance().stop(); }
private:
    AdminManager(){}
};