#include <stdio.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "state.h"
#include "power.h"
#include "wifi.h"
#include "driver/gpio.h"
#include "pins_S3_PHOTOPAINTER.h"
#include "sync.h"
// #include "display.h"

static const char *TAG = "MAIN";

RTC_DATA_ATTR int current_image_index = 0;
RTC_DATA_ATTR bool first_boot_done = false;

static app_state_t state = STATE_BOOT;

// 2 hours deep sleep
// #define SLEEP_US   (2ULL * 60ULL * 60ULL * 1000000ULL)
#define SLEEP_US   (5000000ULL) // 5s for testing

static EventGroupHandle_t heartbeat_event_group;
#define STOP_HEARTBEAT_BIT (1 << 0)

static void heartbeat_task(void *arg)
{
    while (1) {
        EventBits_t bits = xEventGroupGetBits(heartbeat_event_group);

        if (bits & STOP_HEARTBEAT_BIT) {
            break;
        }

        gpio_set_level(PIN_LED_RED, 0);
        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(PIN_LED_RED, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    gpio_set_level(PIN_LED_RED, 1);
    vTaskDelete(NULL);
}

void stop_heartbeat()
{
    xEventGroupSetBits(heartbeat_event_group, STOP_HEARTBEAT_BIT);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== E-Paper Frame Boot ===");

    esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
    switch(reason) {
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wake reason: timer");
            break;
        default:
            ESP_LOGI(TAG, "Wake reason: cold boot");
            break;
    }

    if (!first_boot_done) {
        ESP_LOGI(TAG, "First boot -> init index");
        current_image_index = 0;
        first_boot_done = true;
    }

    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_LED_RED) | (1ULL << PIN_LED_GREEN),
    };
    gpio_config(&io_conf);

    // Start OFF
    gpio_set_level(PIN_LED_RED, 1);
    gpio_set_level(PIN_LED_GREEN, 1);

    heartbeat_event_group = xEventGroupCreate();
    xEventGroupClearBits(heartbeat_event_group, STOP_HEARTBEAT_BIT);
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 5, NULL);

    state = STATE_WIFI_CONNECT;

    while (1)
    {
        switch(state)
        {
            case STATE_WIFI_CONNECT:
                ESP_LOGI(TAG, "STATE: WIFI_CONNECT");
                if (!wifi_connect(10000)) {
                    ESP_LOGE(TAG, "WiFi failed -> sleeping");
                    state = STATE_SLEEP;
                    break;
                }

                state = STATE_SYNC;
                break;

            case STATE_SYNC:
                ESP_LOGI(TAG, "STATE: SYNC");

                gpio_set_level(PIN_LED_GREEN, 0);

                if (!sync_with_remote()) {
                    ESP_LOGW(TAG, "Sync failed -> continuing anyway");
                }
                // vTaskDelay(pdMS_TO_TICKS(5000)); // testing

                gpio_set_level(PIN_LED_GREEN, 1);

                state = STATE_DISPLAY;
                break;

            case STATE_DISPLAY:
                ESP_LOGI(TAG, "STATE: DISPLAY");
                // display logic...

                current_image_index++;
                state = STATE_SLEEP;
                break;

            case STATE_SLEEP:
                ESP_LOGI(TAG, "STATE: SLEEP");

                stop_heartbeat();
                gpio_set_level(PIN_LED_GREEN, 1);
                gpio_set_level(PIN_LED_RED, 1);

                wifi_disconnect();

                vTaskDelay(pdMS_TO_TICKS(1000)); // short settle time

                vTaskDelay(pdMS_TO_TICKS(10000)); // testing
                state = STATE_WIFI_CONNECT;

                // go_to_sleep(SLEEP_US);
                break;

            default:
                ESP_LOGE(TAG, "Invalid state");
                // go_to_sleep(SLEEP_US);
                break;
        }
    }
}