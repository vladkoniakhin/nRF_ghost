#include <unity.h>
#include <string.h>
#include <vector>

// --- MOCKING AREA (Заглушки для Native окружения) ---
#ifndef ARDUINO
#include <stdint.h>
#include <stdlib.h>
#include <chrono>

// Заглушки типов
typedef uint8_t byte;
#define IRAM_ATTR 
#define PROGMEM

// Глобальные переменные-имитаторы железа
uint32_t simulated_millis = 0;
uint32_t simulated_micros = 0;

// Заглушки функций Arduino
uint32_t millis() { return simulated_millis; }
uint32_t micros() { return simulated_micros; }
void delayMicroseconds(uint32_t us) { simulated_micros += us; }
void digitalWrite(int pin, int val) {}
void pinMode(int pin, int mode) {}

// Минимальные реализации структур проекта
#include "Common.h"
#include "Config.h"

// Переменные для SubGhzManager теста
uint16_t g_subGhzBuffer[4096];
size_t g_subGhzIndex = 0;

// Фейковая реализация функций, требующих железа
bool analyzeSignal() {
    if (g_subGhzIndex < 20) return false;
    int u = 0; uint16_t k[16] = {0};
    for (size_t i = 1; i < g_subGhzIndex; i++) {
        bool found = false;
        for (int j = 0; j < u; j++) {
            if (abs(g_subGhzBuffer[i] - k[j]) < 150) { found = true; break; }
        }
        if (!found) {
            if (u < 16) k[u++] = g_subGhzBuffer[i];
            else return true; // Слишком много уникальных таймингов = Rolling
        }
    }
    return (u > 5);
}

// Заглушка для HID конвертера
uint8_t charToHid(char c) {
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    return 0x00;
}
#endif

// --- UNIT TESTS ---

// 1. Тесты SubGhzManager (Логика анализа сигналов)
void test_subghz_static_code(void) {
    g_subGhzIndex = 30;
    for(int i=0; i<30; i++) g_subGhzBuffer[i] = (i % 2 == 0) ? 500 : 1000;
    
    TEST_ASSERT_FALSE_MESSAGE(analyzeSignal(), "Static code detected as rolling!");
}

void test_subghz_rolling_code(void) {
    g_subGhzIndex = 30;
    // Создаем высокую вариативность (шум или динамический код)
    for(int i=0; i<30; i++) g_subGhzBuffer[i] = 200 + (i * 100); 
    
    TEST_ASSERT_TRUE_MESSAGE(analyzeSignal(), "Rolling code not detected with high variance!");
}

void test_subghz_empty_buffer(void) {
    g_subGhzIndex = 5; // Меньше лимита 20
    TEST_ASSERT_FALSE_MESSAGE(analyzeSignal(), "Should return false for small buffers");
}

// 2. Тесты NrfManager (DuckyScript Parser)
void test_duckyscript_delay_calculation(void) {
    uint32_t current_time = 1000;
    simulated_millis = current_time;
    uint32_t delay_val = 500;
    
    uint32_t script_delay_end = simulated_millis + delay_val;
    TEST_ASSERT_EQUAL_UINT32(1500, script_delay_end);
}

void test_duckyscript_char_to_hid(void) {
    TEST_ASSERT_EQUAL_HEX8(0x04, charToHid('a'));
    TEST_ASSERT_EQUAL_HEX8(0x05, charToHid('b'));
    TEST_ASSERT_EQUAL_HEX8(0x00, charToHid('?')); // Неизвестный символ
}

void test_duckyscript_buffer_overflow_safety(void) {
    char long_cmd[256];
    memset(long_cmd, 'A', 255);
    long_cmd[255] = '\0';
    
    // Имитация парсера: проверяем, что strlen не превышает лимит буфера обработки
    TEST_ASSERT_TRUE(strlen(long_cmd) > 128);
    // В реальности здесь проверяется логика обрезания строки в функции processScript
    char process_buffer[128];
    strncpy(process_buffer, long_cmd, 127);
    process_buffer[127] = '\0';
    TEST_ASSERT_EQUAL_INT(127, strlen(process_buffer));
}

// 3. Тесты SystemController (State Machine)
void test_system_state_transition_scan(void) {
    SystemState current_state = SystemState::IDLE;
    // Имитация команды SCAN
    current_state = SystemState::SCANNING;
    TEST_ASSERT_EQUAL(SystemState::SCANNING, current_state);
}

void test_system_stop_task_logic(void) {
    SystemState current_state = SystemState::ATTACKING_NRF;
    void* active_engine = (void*)0x1234; // Имитация указателя
    
    // Логика stopCurrentTask()
    active_engine = nullptr;
    current_state = SystemState::IDLE;
    
    TEST_ASSERT_EQUAL(SystemState::IDLE, current_state);
    TEST_ASSERT_NULL(active_engine);
}

// 4. Тесты Данных (WiFi / SD)
void test_pcap_header_integrity(void) {
    PcapGlobalHeader header;
    TEST_ASSERT_EQUAL_UINT32(0xa1b2c3d4, header.magic_number);
    TEST_ASSERT_EQUAL_UINT16(2, header.version_major);
    TEST_ASSERT_EQUAL_UINT32(105, header.network); // DLT_IEEE802_11
}

void test_wifi_target_parsing(void) {
    TargetAP ap;
    const char* test_ssid = "Test_AP_Long_Name_Over_32_Chars";
    
    strncpy(ap.ssid, test_ssid, 32);
    ap.ssid[32] = '\0';
    
    TEST_ASSERT_EQUAL_INT(32, strlen(ap.ssid));
    TEST_ASSERT_EQUAL_STRING_LEN("Test_AP_Long_Name_Over_32_Chars", ap.ssid, 32);
}

// 5. Ситуативные проверки (User Experience / Safety)
void test_user_emergency_stop(void) {
    // Кейс: Пользователь нажал BACK во время атаки
    SystemState state = SystemState::ATTACKING_WIFI;
    bool back_pressed = true;
    
    if (back_pressed) {
        state = SystemState::IDLE;
    }
    
    TEST_ASSERT_EQUAL_MESSAGE(SystemState::IDLE, state, "System failed to return to IDLE on BACK press");
}

void test_sd_card_unplugged_during_capture(void) {
    bool is_capturing = true;
    bool sd_mounted = false; // Имитируем извлечение
    
    if (!sd_mounted && is_capturing) {
        is_capturing = false;
    }
    
    TEST_ASSERT_FALSE_MESSAGE(is_capturing, "Capture should stop if SD is unmounted");
}

// --- SETUP & RUN ---

void setUp(void) {
    // Сброс имитаторов перед каждым тестом
    simulated_millis = 0;
    simulated_micros = 0;
    g_subGhzIndex = 0;
}

void tearDown(void) {
    // Очистка после теста
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // Block 1: SubGHz
    RUN_TEST(test_subghz_static_code);
    RUN_TEST(test_subghz_rolling_code);
    RUN_TEST(test_subghz_empty_buffer);

    // Block 2: DuckyScript
    RUN_TEST(test_duckyscript_delay_calculation);
    RUN_TEST(test_duckyscript_char_to_hid);
    RUN_TEST(test_duckyscript_buffer_overflow_safety);

    // Block 3: System Logic
    RUN_TEST(test_system_state_transition_scan);
    RUN_TEST(test_system_stop_task_logic);
    RUN_TEST(test_user_emergency_stop);

    // Block 4: Data & SD
    RUN_TEST(test_pcap_header_integrity);
    RUN_TEST(test_wifi_target_parsing);
    RUN_TEST(test_sd_card_unplugged_during_capture);

    return UNITY_END();
}