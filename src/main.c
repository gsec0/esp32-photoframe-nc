#include "system.h"

QueueHandle_t download_queue = NULL;
EventGroupHandle_t system_events = NULL;

void sync_task(void *arg);
void download_task(void *arg);

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

void download_task(void *arg)
{
    ESP_LOGI("DOWNLOAD", "Download task started");

    download_job_t job;

    while (true) {

        if (xQueueReceive(
                download_queue,
                &job,
                portMAX_DELAY)) {

            ESP_LOGI("DOWNLOAD",
                "Download job: %s",
                job.filename);

            // future download logic
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

void sd_init(void)
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
        &host,          // <‑‑ missing in your code
        &slot_config,
        &mount_config,
        &card);

    if (ret != ESP_OK) {
        ESP_LOGE("SD", "SD mount failed: %s", esp_err_to_name(ret));
        return;
    }

    sdmmc_card_print_info(stdout, card);

    xEventGroupSetBits(system_events, SD_READY_BIT);

    ESP_LOGI("SD", "SD card mounted");
}

static void hardware_init(void)
{
    wifi_init();
    sd_init();
}

static void extract_filename(const char *href)
{
    const char *p = strrchr(href, '/');
    if (!p) return;

    p++;

    if (*p == '\0') return;

    if (remote_file_count >= MAX_REMOTE_FILES)
        return;

    strncpy(remote_files[remote_file_count], p, MAX_FILENAME - 1);
    remote_files[remote_file_count][MAX_FILENAME-1] = 0;

    ESP_LOGI("SYNC", "Remote file: %s", remote_files[remote_file_count]);

    remote_file_count++;
}

static void parse_xml_chunk(char *data, int len)
{
    // Prepend any overlap from previous chunk
    char temp_buf[768];  // data + overlap
    int total_len = overlap_len;
    
    if (total_len + len > sizeof(temp_buf) - 1) {
        total_len = 0;  // Overflow, reset
        overlap_len = 0;
    }
    
    if (total_len > 0) {
        memcpy(temp_buf, overlap_buf, overlap_len);
    }
    memcpy(temp_buf + total_len, data, len);
    total_len += len;
    temp_buf[total_len] = 0;
    
    char *p = temp_buf;
    while ((p = strstr(p, "<d:href>"))) {
        p += 8;
        char *end = strstr(p, "</d:href>");
        if (!end) {
            // Incomplete tag, save overlap
            overlap_len = total_len - (p - temp_buf);
            memcpy(overlap_buf, p, overlap_len);
            return;
        }
        
        char href[256];
        int l = end - p;
        if (l >= sizeof(href)) l = sizeof(href) - 1;
        
        memcpy(href, p, l);
        href[l] = 0;
        
        extract_filename(href);
        // remote_file_count++;  // Track count here
        
        p = end + 9;
    }
    
    // No pending overlap
    overlap_len = 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE("HTTP", "HTTP_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI("HTTP", "Connected");
            break;
        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len) {
                parse_xml_chunk((char*)evt->data, evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static bool fetch_remote_file_list(void)
{
    remote_file_count = 0;

    esp_http_client_config_t config = {
        .url = WEBDAV_URL,
        .method = HTTP_METHOD_PROPFIND,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_header(client, "Depth", "1");
    esp_http_client_set_header(client, "Content-Type", "application/xml");

    esp_http_client_set_post_field(
        client,
        propfind_body,
        strlen(propfind_body));

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {

        ESP_LOGE("SYNC", "HTTP request failed");

        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);

    ESP_LOGI("SYNC", "HTTP status: %d", status);

    esp_http_client_cleanup(client);

    ESP_LOGI("SYNC", "Parsed %d remote files", remote_file_count);

    return true;
}

void sync_task(void *arg)
{
    ESP_LOGI("SYNC", "Sync task started");

    xEventGroupWaitBits(
        system_events,
        WIFI_CONNECTED_BIT | SD_READY_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI("SYNC", "System ready");

    while (true) {

        ESP_LOGI("SYNC", "Fetching remote file list");

        fetch_remote_file_list();

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

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