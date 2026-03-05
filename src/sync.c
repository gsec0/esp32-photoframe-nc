#include "sync.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "pins_S3_PHOTOPAINTER.h"

#define TAG "SYNC"

// ==== CONFIG ====
#define WEBDAV_URL "https://192.168.1.63:7070/remote.php/dav/photospublic/yJ4UmAGcRkslSl2XoQ0MR2mt3n8PacX3/"
#define CONVERT_URL "http://192.168.1.63:9600/convert?url="
#define STORAGE_PATH "/sdcard/pictures/"

// ==== LIMITS ====
#define MAX_FILES 100
#define MAX_FILENAME 128
#define XML_BUFFER_SIZE 8192

static char remote_files[MAX_FILES][MAX_FILENAME];
static int remote_count = 0;

static char local_files[MAX_FILES][MAX_FILENAME];
static int local_count = 0;

static bool sd_initialized = false;

static bool sdcard_init(void)
{
    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdspi_mount(
        "/sdcard",
        &host,
        &slot_config,
        &mount_config,
        &card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed");
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted");

    if (mkdir("/sdcard/pictures", 0775) != 0) {
        if (errno == EEXIST) {
            ESP_LOGI(TAG, "Directory already exists");
        } else {
            ESP_LOGW(TAG, "mkdir failed: %d", errno);
        }
    }

    return true;
}

static void to_bmp_name(char *out, const char *in)
{
    const char *dot = strrchr(in, '.');
    size_t len = dot ? (size_t)(dot - in) : strlen(in);

    snprintf(out, MAX_FILENAME, "%.*s.bmp", (int)len, in);
}

// HTTP: Fetch XML
static bool fetch_xml(char *buffer, size_t max_len)
{
    esp_http_client_config_t config = {
        .url = WEBDAV_URL,
        .method = HTTP_METHOD_PROPFIND,
        .timeout_ms = 10000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = NULL,
        .use_global_ca_store = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Depth", "1");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP PROPFIND failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int len = esp_http_client_read_response(client, buffer, max_len - 1);
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to read response");
        esp_http_client_cleanup(client);
        return false;
    }
    buffer[len] = 0;

    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Fetched XML (%d bytes)", len);
    return true;
}

// XML parsing (simple)
static void parse_xml(char *xml)
{
    remote_count = 0;

    char *ptr = xml;

    while ((ptr = strstr(ptr, "<d:href>")) && remote_count < MAX_FILES)
    {
        ptr += strlen("<d:href>");

        char *end = strstr(ptr, "</d:href>");
        if (!end) break;

        char full_path[256];
        int len = end - ptr;
        if (len >= sizeof(full_path)) len = sizeof(full_path) - 1;
        memcpy(full_path, ptr, len);
        full_path[len] = 0;

        // Extract filename
        char *last_slash = strrchr(full_path, '/');
        if (last_slash && *(last_slash + 1) != '\0')
        {
            strncpy(remote_files[remote_count], last_slash + 1, MAX_FILENAME - 1);
            remote_files[remote_count][MAX_FILENAME - 1] = '\0';
            remote_count++;
        }

        ptr = end;
    }

    ESP_LOGI(TAG, "Parsed %d remote files", remote_count);
}

// Read local SD files
static void read_local_files(void)
{
    local_count = 0;

    DIR *dir = opendir(STORAGE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir");
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) && local_count < MAX_FILES)
    {
        if (entry->d_type == DT_REG)
        {
            strncpy(local_files[local_count], entry->d_name, MAX_FILENAME);
            local_count++;
        }
    }

    closedir(dir);

    ESP_LOGI(TAG, "Found %d local files", local_count);
}

// Helpers
static bool file_exists_in_list(char *name, char list[][MAX_FILENAME], int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(name, list[i]) == 0) return true;
    }
    return false;
}

static void delete_extra_files(void)
{
    char bmp_name[MAX_FILENAME];
    bool found;

    for (int i = 0; i < local_count; i++)
    {
        found = false;

        for (int j = 0; j < remote_count; j++)
        {
            to_bmp_name(bmp_name, remote_files[j]);

            if (strcmp(local_files[i], bmp_name) == 0) {
                found = true;
                break;
            }
        }

        if (!found)
        {
            char path[256];
            snprintf(path, sizeof(path), "%s%s", STORAGE_PATH, local_files[i]);

            ESP_LOGI(TAG, "Deleting: %s", path);
            unlink(path);
        }
    }
}

// Download file via microservice
static bool download_file(const char *filename)
{
    char url[512];
    snprintf(url, sizeof(url),
        "%s%s%s",
        CONVERT_URL,
        WEBDAV_URL,
        filename
    );

    ESP_LOGI(TAG, "Downloading: %s", filename);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (esp_http_client_perform(client) != ESP_OK) {
        ESP_LOGE(TAG, "Download failed");
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status);
        esp_http_client_cleanup(client);
        return false;
    }

    char path[256];
    char bmp_name[MAX_FILENAME];
    to_bmp_name(bmp_name, filename);

    snprintf(path, sizeof(path), "%s%s", STORAGE_PATH, bmp_name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "File open failed");
        esp_http_client_cleanup(client);
        return false;
    }

    char buffer[1024];
    int read_len;

    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, read_len, f);
    }

    fclose(f);
    esp_http_client_cleanup(client);

    return true;
}

// Download missing files
static void download_missing_files(void)
{
    char bmp_name[MAX_FILENAME];

    for (int i = 0; i < remote_count; i++)
    {
        to_bmp_name(bmp_name, remote_files[i]);

        if (!file_exists_in_list(bmp_name, local_files, local_count))
        {
            download_file(remote_files[i]);
        }
    }
}

// PUBLIC ENTRY
bool sync_with_remote(void)
{
    if (!sd_initialized) {
        sd_initialized = sdcard_init();
        if (!sd_initialized) {
            ESP_LOGE(TAG, "SD init failed");
            return false;
        }
    }

    char *xml = malloc(XML_BUFFER_SIZE);
    if (!xml) return false;

    if (!fetch_xml(xml, XML_BUFFER_SIZE)) {
        free(xml);
        return false;
    }

    parse_xml(xml);
    free(xml);

    read_local_files();
    delete_extra_files();
    download_missing_files();

    ESP_LOGI(TAG, "Sync complete");

    return true;
}