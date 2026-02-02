#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_spiMutex;

namespace Config {
    // --- PINS (MH-ET LIVE ESP32) ---
    constexpr uint8_t PIN_OLED_SDA = 21;
    constexpr uint8_t PIN_OLED_SCL = 22;
    constexpr uint8_t PIN_BTN_UP       = 26;
    constexpr uint8_t PIN_BTN_DOWN     = 32;
    constexpr uint8_t PIN_BTN_SELECT   = 33;
    constexpr uint8_t PIN_BTN_LEFT     = 25;
    constexpr uint8_t PIN_BTN_RIGHT    = 2;
    constexpr uint8_t PIN_NEOPIXEL     = 14;
    constexpr uint8_t NEOPIXEL_COUNT   = 1;

    // SPI Bus
    constexpr uint8_t PIN_SPI_SCK  = 18;
    constexpr uint8_t PIN_SPI_MISO = 19;
    constexpr uint8_t PIN_SPI_MOSI = 23;

    // Radio Modules
    constexpr uint8_t PIN_NRF_CE_A  = 5;
    constexpr uint8_t PIN_NRF_CSN_A = 17;
    constexpr uint8_t PIN_NRF_CE_B  = 16;
    constexpr uint8_t PIN_NRF_CSN_B = 4; 
    constexpr uint8_t PIN_CC_CS     = 27;
    constexpr uint8_t PIN_CC_GDO0   = 15;
    constexpr uint8_t PIN_SD_CS     = 13;
    constexpr uint8_t PIN_BAT_ADC   = 34;

    // --- SYSTEM CONSTANTS ---
    constexpr size_t PCAP_QUEUE_SIZE = 64;
    constexpr size_t MAX_PACKET_LEN  = 256;
    constexpr uint32_t SPI_SPEED_MHZ = 10000000;
    constexpr uint32_t SERIAL_BAUD   = 115200;

    // --- ATTACK SETTINGS ---
    constexpr size_t RAW_BUFFER_SIZE = 4096;
    constexpr uint32_t SIGNAL_TIMEOUT_US = 50000;
    constexpr float SUBGHZ_SCAN_STEP = 0.05;
    constexpr uint32_t NRF_JAMMING_DELAY_US = 20;
    
    // v5.2 Refactoring Constants
    constexpr float FSK_DEVIATION_DEFAULT = 47.6;
    constexpr uint16_t CAME_BIT_PERIOD = 320;
    constexpr uint32_t SUBGHZ_STACK_SIZE = 10240; // Increased stack
}