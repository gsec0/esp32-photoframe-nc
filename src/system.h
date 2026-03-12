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

// Sync
#define MAX_REMOTE_FILES 256
#define MAX_FILENAME     128

static char remote_files[MAX_REMOTE_FILES][MAX_FILENAME];
static int remote_file_count = 0;

// Small 512-byte ring buffer for tag overlap across chunks
static char overlap_buf[512];
static int overlap_len = 0;

#include "esp_http_client.h"

#define WEBDAV_URL "https://192.168.1.63:7070/remote.php/dav/photospublic/yJ4UmAGcRkslSl2XoQ0MR2mt3n8PacX3/"
static const char *propfind_body =
"<?xml version=\"1.0\"?>"
"<d:propfind xmlns:d=\"DAV:\">"
"<d:prop>"
"<d:resourcetype/>"
"</d:prop>"
"</d:propfind>";