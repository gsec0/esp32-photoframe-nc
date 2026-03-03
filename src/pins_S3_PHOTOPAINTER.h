#pragma once

#include "driver/gpio.h"

/*
 * Waveshare ESP32-S3 PhotoPainter Pin Definitions
 * Based on official schematic / provided table
 */

// =========================
// User Input / Control
// =========================
#define PIN_USER_KEY1        GPIO_NUM_4     // Active LOW button
#define PIN_SYS_OUT_CTRL     GPIO_NUM_5     // AXP2101 SYS_OUT control
#define PIN_RTC_INT          GPIO_NUM_6     // PCF85063 interrupt
#define PIN_AUDIO_CTRL       GPIO_NUM_7     // Audio amp enable (NS4150B)

// =========================
// I2C Bus (Shared)
// =========================
#define PIN_I2C_SDA          GPIO_NUM_47
#define PIN_I2C_SCL          GPIO_NUM_48

// =========================
// PMIC Interrupt
// =========================
#define PIN_AXP_IRQ          GPIO_NUM_21

// =========================
// Status LEDs
// =========================
#define PIN_LED_RED          GPIO_NUM_45
#define PIN_LED_GREEN        GPIO_NUM_42
#define PIN_CHG_LED          GPIO_NUM_3      // Controlled by PMIC

// =========================
// UART
// =========================
#define PIN_UART_RX          GPIO_NUM_43
#define PIN_UART_TX          GPIO_NUM_44

// =========================
// ePaper Display (SPI)
// =========================
#define PIN_EPD_CS           GPIO_NUM_9
#define PIN_EPD_SCK          GPIO_NUM_10
#define PIN_EPD_MOSI         GPIO_NUM_11
#define PIN_EPD_DC           GPIO_NUM_8
#define PIN_EPD_RST          GPIO_NUM_12
#define PIN_EPD_BUSY         GPIO_NUM_13

// =========================
// SD Card (SPI Mode)
// =========================
#define PIN_SD_CS            GPIO_NUM_38
#define PIN_SD_CLK           GPIO_NUM_39
#define PIN_SD_MISO          GPIO_NUM_40
#define PIN_SD_MOSI          GPIO_NUM_41

// Optional SD lines (4-bit mode, not used in SPI)
#define PIN_SD_D1            GPIO_NUM_1
#define PIN_SD_D2            GPIO_NUM_2

// =========================
// I2S Audio (Unused for now)
// =========================
#define PIN_I2S_SCLK         GPIO_NUM_15
#define PIN_I2S_LRCK         GPIO_NUM_16
#define PIN_I2S_DIN          GPIO_NUM_17
#define PIN_I2S_DOUT         GPIO_NUM_18
#define PIN_I2S_MCLK         GPIO_NUM_14

// =========================
// USB (Native)
// =========================
#define PIN_USB_D_MINUS      GPIO_NUM_19
#define PIN_USB_D_PLUS       GPIO_NUM_20

// =========================
// Boot / System
// =========================
#define PIN_BOOT             GPIO_NUM_0

// =========================
// Unused / Free GPIO
// =========================
#define PIN_GPIO_FREE_1      GPIO_NUM_46
#define PIN_GPIO_FREE_2      GPIO_NUM_35
#define PIN_GPIO_FREE_3      GPIO_NUM_36
#define PIN_GPIO_FREE_4      GPIO_NUM_37
