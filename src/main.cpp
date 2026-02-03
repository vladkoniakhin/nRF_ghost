#include <Arduino.h>
#include "System.h"

TaskHandle_t g_TaskUI = nullptr;
TaskHandle_t g_TaskWorker = nullptr;

// Main processing core
void TaskWorker(void* pvParameters) {
    SystemController::getInstance().init();
    SystemController::getInstance().runWorkerLoop();
    vTaskDelete(NULL);
}

// UI/Display core
void TaskUI(void* pvParameters) {
    // Вся логика UI теперь внутри System.cpp::TaskUI
    // Но мы должны вызвать эту функцию отсюда
    // Так как сигнатура должна совпадать с FreeRTOS TaskFunction_t
    
    // В данном проекте реализация TaskUI находится в System.cpp как глобальная функция
    // Поэтому здесь мы просто объявляем extern (в хедере System.h это уже сделано)
    // Но чтобы не было конфликтов линковки, код UI перенесен в System.cpp полностью.
    // main.cpp только запускает задачи.
}

// Реальная точка входа Arduino
void setup() {
    Serial.begin(115200);
    
    // Core 0: Worker (Radio, Attacks, File IO)
    xTaskCreatePinnedToCore(TaskWorker, "Worker", 10000, NULL, 1, &g_TaskWorker, 0);
    
    // Core 1: UI (Display, Input, LED)
    // Функция TaskUI определена в System.cpp и экспортирована через System.h
    xTaskCreatePinnedToCore(TaskUI, "UI", 5000, NULL, 1, &g_TaskUI, 1);
    
    // Удаляем задачу loop(), чтобы освободить память в стеке main
    vTaskDelete(NULL);
}

void loop() {
    // Не используется
}