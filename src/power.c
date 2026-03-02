#include "power.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "POWER";

void go_to_sleep(uint64_t us)
{
    ESP_LOGI(TAG, "Sleeping for %.1f minutes...",
             (double)us / 60000000.0);

    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
}