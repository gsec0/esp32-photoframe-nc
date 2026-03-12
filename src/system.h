#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "pins_S3_PHOTOPAINTER.h"

#define WIFI_CONNECTED_BIT BIT0
#define SD_READY_BIT       BIT1
#define SYNC_REQUEST_BIT   BIT2

typedef struct {
    char filename[128];
} download_job_t;

extern QueueHandle_t download_queue;
extern EventGroupHandle_t system_events;

// SD and WiFi
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"

#define WIFI_SSID "Namai_2G"
#define WIFI_PASS "Germuska18"

#include "sync.h"
#include "errno.h"
#include <sys/stat.h>