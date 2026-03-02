#include <stdio.h>
#include "esp_log.h"
#include "esp_sleep.h"

#include "state.h"
#include "power.h"
#include "wifi.h"
#include "sync.h"
#include "display.h"

static const char *TAG = "MAIN";

RTC_DATA_ATTR int current_image_index = 0;
RTC_DATA_ATTR bool first_boot_done = false;

static app_state_t state = STATE_BOOT;

// 2 hours deep sleep
#define SLEEP_US   (2ULL * 60ULL * 60ULL * 1000000ULL)

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
        ESP_LOGI(TAG, "First boot → init index");
        current_image_index = 0;
        first_boot_done = true;
    }

    state = STATE_WIFI_CONNECT;

    while (1)
    {
        switch(state)
        {
            case STATE_WIFI_CONNECT:
                ESP_LOGI(TAG, "STATE: WIFI_CONNECT");
                if (!wifi_connect(10000)) {
                    ESP_LOGE(TAG, "WiFi failed → sleeping");
                    go_to_sleep(SLEEP_US);
                }
                state = STATE_SYNC;
                break;

            case STATE_SYNC:
                ESP_LOGI(TAG, "STATE: SYNC");
                if (!sync_with_remote(current_image_index)) {
                    ESP_LOGW(TAG, "Sync failed → continuing anyway");
                }
                state = STATE_DISPLAY;
                break;

            case STATE_DISPLAY:
                ESP_LOGI(TAG, "STATE: DISPLAY");
                if (!display_image(current_image_index)) {
                    ESP_LOGE(TAG, "Display failed");
                }

                current_image_index++;
                state = STATE_SLEEP;
                break;

            case STATE_SLEEP:
                ESP_LOGI(TAG, "STATE: SLEEP");
                go_to_sleep(SLEEP_US);
                break;

            default:
                ESP_LOGE(TAG, "Invalid state");
                go_to_sleep(SLEEP_US);
                break;
        }
    }
}