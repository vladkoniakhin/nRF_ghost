#include <Arduino.h>
#include "System.h"
#include "DisplayManager.h"
#include "InputManager.h"
#include "LedManager.h"

TaskHandle_t g_TaskUI = nullptr;
TaskHandle_t g_TaskWorker = nullptr;

void TaskWorker(void* pvParameters) {
    SystemController::getInstance().init();
    SystemController::getInstance().runWorkerLoop();
    vTaskDelete(NULL);
}

void TaskUI(void* pvParameters) {
    auto& input = InputManager::getInstance();
    auto& display = DisplayManager::getInstance();
    auto& sys = SystemController::getInstance();
    auto& leds = LedManager::getInstance();

    // Инициализация
    input.init(); 
    display.init(); 
    leds.init();
    
    display.showSplashScreen(); 
    vTaskDelay(pdMS_TO_TICKS(1500));

    StatusMessage statusMsg; 
    statusMsg.state = SystemState::IDLE;
    CommandMessage cmdOut;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        // 1. Опрос кнопок
        InputEvent evt = input.poll();
        if (evt != InputEvent::NONE) {
            display.handleInput(evt);
            
            // Логика меню (UI)
            if (evt == InputEvent::BTN_SELECT) {
                if (statusMsg.state == SystemState::IDLE) {
                    int idx = display.getMenuIndex(); cmdOut.param1 = 0;
                    
                    if (idx == 0) cmdOut.cmd = SystemCommand::CMD_START_SCAN_WIFI;
                    else if (idx == 1) cmdOut.cmd = SystemCommand::CMD_START_DEAUTH;
                    else if (idx == 2) cmdOut.cmd = SystemCommand::CMD_START_BEACON_SPAM;
                    else if (idx == 3) cmdOut.cmd = SystemCommand::CMD_START_EVIL_TWIN;
                    else if (idx == 4) { statusMsg.state = SystemState::MENU_SELECT_BLE; display.resetSubmenuIndex(); display.updateStatus(statusMsg); }
                    else if (idx == 5) { statusMsg.state = SystemState::MENU_SELECT_NRF; display.resetSubmenuIndex(); display.updateStatus(statusMsg); }
                    else if (idx == 6) cmdOut.cmd = SystemCommand::CMD_START_NRF_JAM;
                    else if (idx == 7) cmdOut.cmd = SystemCommand::CMD_START_NRF_ANALYZER;
                    else if (idx == 8) cmdOut.cmd = SystemCommand::CMD_START_NRF_SNIFF;
                    else if (idx == 9) cmdOut.cmd = SystemCommand::CMD_START_MOUSEJACK;
                    else if (idx == 10) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_SCAN;
                    else if (idx == 11) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_JAM;
                    else if (idx == 12) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_RX; 
                    else if (idx == 13) cmdOut.cmd = SystemCommand::CMD_START_SUBGHZ_TX; 
                    else if (idx == 14) cmdOut.cmd = SystemCommand::CMD_START_ADMIN_MODE;
                    else if (idx == 15) cmdOut.cmd = SystemCommand::CMD_STOP_ATTACK;
                    
                    // Отправляем команду, только если не в подменю
                    if (statusMsg.state == SystemState::IDLE) sys.sendCommand(cmdOut);
                } 
                else if (statusMsg.state == SystemState::SCAN_COMPLETE) {
                    cmdOut.cmd = SystemCommand::CMD_SELECT_TARGET; cmdOut.param1 = display.getTargetIndex(); sys.sendCommand(cmdOut);
                    cmdOut.cmd = SystemCommand::CMD_START_DEAUTH; cmdOut.param1 = 0; sys.sendCommand(cmdOut);
                }
            } else if (evt == InputEvent::BTN_BACK) {
                cmdOut.cmd = SystemCommand::CMD_STOP_ATTACK; sys.sendCommand(cmdOut);
            }
        }
        
        // 2. Получение статуса от ядра
        StatusMessage newMsg;
        if (sys.getStatus(newMsg)) {
            statusMsg = newMsg; 
            display.updateStatus(statusMsg); 
            
            // FIXED: New LED API v3.1
            leds.setStatus(statusMsg); 
            
            if (statusMsg.state == SystemState::SCAN_COMPLETE) display.setTargetList(sys.getScanResults());
        }
        
        // 3. Обновление периферии (анимации и отрисовка)
        leds.update(); // Вызываем в каждом цикле для плавных эффектов
        display.render();
        
        // 30 FPS UI
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(33));
    }
}

void setup() {
    Serial.begin(115200);
    // Запуск задач на разных ядрах для стабильности
    xTaskCreatePinnedToCore(TaskWorker, "Worker", 10000, NULL, 1, &g_TaskWorker, 0);
    xTaskCreatePinnedToCore(TaskUI, "UI", 5000, NULL, 1, &g_TaskUI, 1);
    vTaskDelete(NULL);
}

void loop() {}