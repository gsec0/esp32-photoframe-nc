#include "system.h"

QueueHandle_t download_queue = NULL;
EventGroupHandle_t system_events = NULL;

static void system_init(void);
static void start_tasks(void);
void download_task(void *arg);
void hardware_init(void);
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

void app_main(void)
{
    ESP_LOGI("MAIN", "Firmware boot");

    system_init();

    hardware_init();

    start_tasks();

    while (true) {

        ESP_LOGI("MAIN",
                 "Free heap: %d",
                 esp_get_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void system_init(void)
{
    ESP_LOGI("SYSTEM", "Initializing system");

    system_events = xEventGroupCreate();
    assert(system_events);

    download_queue = xQueueCreate(
        8,                      // queue length
        sizeof(download_job_t)  // item size
    );
    assert(download_queue);
}

static void start_tasks(void)
{
    xTaskCreatePinnedToCore(
        sync_task,
        "sync_task",
        12288,
        NULL,
        5,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        download_task,
        "download_task",
        16384,
        NULL,
        5,
        NULL,
        0
    );
}

bool download_file_stub_create_empty_bmp(const char *remote_filename)
{
    char bmp_name[MAX_FILENAME];
    to_bmp_name(bmp_name, remote_filename);

    char path[256];
    snprintf(path, sizeof(path), "%s%s", STORAGE_PATH, bmp_name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE("DOWNLOAD_STUB", "Failed to open %s for write: %s", path, strerror(errno));
        return false;
    }

    // Write a tiny BMP header / zero bytes or leave empty — just create file
    fclose(f);

    ESP_LOGI("DOWNLOAD_STUB", "Created stub file %s", path);
    return true;
}

void download_task(void *arg)
{
    ESP_LOGI("DOWNLOAD", "Download task started");

    download_job_t job;

    while (true) {
        if (xQueueReceive(download_queue, &job, portMAX_DELAY) == pdTRUE) {

            ESP_LOGI("DOWNLOAD", "Starting download job: %s", job.filename);

            // Call the actual downloader. We'll implement a proper streaming downloader next.
            // For now use a safe stub that writes an empty .bmp file to SD so you can test sync.
            extern bool download_file_stub_create_empty_bmp(const char *remote_filename); // declare below or in header
            bool ok = download_file_stub_create_empty_bmp(job.filename);

            if (!ok) {
                ESP_LOGW("DOWNLOAD", "download failed for %s", job.filename);
                // Optionally re-enqueue with backoff, or record failure for retry
            } else {
                ESP_LOGI("DOWNLOAD", "download succeeded: %s", job.filename);
            }

            ESP_LOGI("DOWNLOAD", "Free heap: %u ; download task high water: %u",
                    esp_get_free_heap_size(),
                    uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        ESP_LOGI("WIFI", "Disconnected, reconnecting");
        esp_wifi_connect();
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event = event_data;

        ESP_LOGI("WIFI",
                 "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        xEventGroupSetBits(system_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    ESP_LOGI("WIFI", "Initializing WiFi");

    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WIFI", "WiFi started");
}

static void sd_init(void)
{
    ESP_LOGI("SD", "Initializing SD card");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA));

    // SD‑SPI host config
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;          // use same SPI host as spi_bus_initialize

    // SD‑SPI device / slot config
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        "/sdcard",
        &host,
        &slot_config,
        &mount_config,
        &card);

    if (ret != ESP_OK) {
        ESP_LOGE("SD", "SD mount failed: %s", esp_err_to_name(ret));
        return;
    }

    sdmmc_card_print_info(stdout, card);

    if (mkdir("/sdcard/pictures", 0775) != 0) {
        if (errno == EEXIST) {
            ESP_LOGI("SD", "Directory /sdcard/pictures already exists");
        } else {
            ESP_LOGW("SD", "mkdir /sdcard/pictures failed: %s", strerror(errno));
        }
    } else {
        ESP_LOGI("SD", "Created /sdcard/pictures");
    }

    xEventGroupSetBits(system_events, SD_READY_BIT);

    ESP_LOGI("SD", "SD card mounted");
}

void hardware_init(void)
{
    wifi_init();
    sd_init();
}