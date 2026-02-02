#pragma once
#include <Arduino.h>
#include <Update.h>
#include <SD.h>
#include "Config.h"
#include "DisplayManager.h"

// Класс управления OTA обновлениями с SD карты
class UpdateManager {
public:
    static void performUpdateIfAvailable() {
        // Проверка наличия карты и файла update.bin
        if (!SD.begin(Config::PIN_SD_CS)) return;
        
        File bin = SD.open("/update.bin");
        if (bin && !bin.isDirectory()) {
            // Если файл найден, выводим уведомление на экран
            DisplayManager::getInstance().init();
            DisplayManager::getInstance().drawPopup("Firmware Found\nUpdating...");
            DisplayManager::getInstance().render();
            
            size_t fileSize = bin.size();
            
            if (Update.begin(fileSize, U_FLASH)) {
                size_t written = Update.writeStream(bin);
                
                if (written == fileSize && Update.end()) {
                    if (Update.isFinished()) {
                        DisplayManager::getInstance().drawPopup("Success! Rebooting...");
                        DisplayManager::getInstance().render();
                        delay(2000);
                        
                        bin.close();
                        // Переименовываем файл, чтобы не шить по кругу
                        SD.rename("/update.bin", "/update.bak");
                        
                        ESP.restart();
                    }
                } else {
                    Serial.printf("Update Error: %d\n", Update.getError());
                }
            }
            bin.close();
        }
    }
};