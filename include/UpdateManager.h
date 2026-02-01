#pragma once
#include <Arduino.h>
#include <Update.h>
#include <SD.h>
#include "Config.h"
#include "DisplayManager.h"

class UpdateManager {
public:
    static void performUpdateIfAvailable() {
        if (!SD.begin(Config::PIN_SD_CS)) return;
        File bin = SD.open("/update.bin");
        if (bin && !bin.isDirectory()) {
            DisplayManager::getInstance().init();
            DisplayManager::getInstance().drawPopup("Firmware Found\nUpdating...");
            DisplayManager::getInstance().render();
            if (Update.begin(bin.size(), U_FLASH)) {
                Update.writeStream(bin);
                if (Update.end() && Update.isFinished()) {
                    DisplayManager::getInstance().drawPopup("Success! Rebooting...");
                    DisplayManager::getInstance().render();
                    delay(2000); bin.close(); SD.rename("/update.bin", "/update.bak"); ESP.restart();
                }
            }
            bin.close();
        }
    }
};