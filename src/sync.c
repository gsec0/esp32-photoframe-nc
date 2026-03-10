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
#define XML_BUFFER_SIZE 32768

static char remote_files[MAX_FILES][MAX_FILENAME];
static int remote_count = 0;

static char local_files[MAX_FILES][MAX_FILENAME];
static int local_count = 0;

static bool sd_initialized = false;

// // Add this event handler
// esp_err_t http_event_handler(void *handler_args, esp_http_client_event_t *evt)
// {
//     char **buf = (char**)evt->user_data;
    
//     switch(evt->event_id) {
//         case HTTP_EVENT_ON_DATA:
//             if (evt->data_len > 0) {
//                 int avail = XML_BUFFER_SIZE - strlen(*buf);
//                 if (avail > 0) {
//                     int copy_len = (evt->data_len < avail) ? evt->data_len : avail - 1;
//                     memcpy(*buf + strlen(*buf), evt->data, copy_len);
//                     (*buf)[strlen(*buf) + copy_len] = '\0';
//                 }
//             }
//             break;
//         default:
//             break;
//     }
//     return ESP_OK;
// }

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
// small container for response assembly
typedef struct {
    char *buf;
    size_t max_len;
    size_t len;
} http_resp_ctx_t;

// event handler: append data chunks into ctx->buf
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx && evt->data && evt->data_len > 0) {
            // copy up to available space (leave room for null)
            size_t room = (ctx->max_len - 1) - ctx->len;
            size_t to_copy = evt->data_len;
            if (to_copy > room) to_copy = room;
            if (to_copy > 0) {
                memcpy(ctx->buf + ctx->len, evt->data, to_copy);
                ctx->len += to_copy;
            } // else: buffer full, drop remainder
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool fetch_xml(char *buffer, size_t max_len)
{
    http_resp_ctx_t ctx = { .buf = buffer, .max_len = max_len, .len = 0 };

    esp_http_client_config_t config = {
        .url = WEBDAV_URL,
        .method = HTTP_METHOD_PROPFIND,
        .timeout_ms = 10000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
        .event_handler = http_event_handler,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return false;
    }

    // Headers: Depth is required for WebDAV; some servers prefer an explicit zero content length
    esp_http_client_set_header(client, "Depth", "1");
    esp_http_client_set_header(client, "Content-Type", "application/xml");
    esp_http_client_set_header(client, "Content-Length", "0");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP PROPFIND failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", status);

    // ctx.len holds how many bytes the event handler collected
    ctx.buf[ (ctx.len < max_len) ? ctx.len : (max_len - 1) ] = '\0';
    ESP_LOGI(TAG, "Fetched XML (%u bytes)", (unsigned)ctx.len);
    ESP_LOGI(TAG, "XML snippet: '%.200s'", ctx.buf);

    esp_http_client_cleanup(client);
    return (ctx.len > 0 && (status == 200 || status == 207));
}

// XML parsing (simple)
static void parse_xml(char *xml)
{
    remote_count = 0;

    char *resp = xml;

    while ((resp = strstr(resp, "<d:response>")) && remote_count < MAX_FILES)
    {
        char *resp_end = strstr(resp, "</d:response>");
        if (!resp_end) break;

        // Check if this response is a collection (directory)
        if (strstr(resp, "<d:collection/>") && strstr(resp, "<d:collection/>") < resp_end)
        {
            resp = resp_end + 1;
            continue;
        }

        // Find href inside this response
        char *href = strstr(resp, "<d:href>");
        if (!href || href > resp_end) {
            resp = resp_end + 1;
            continue;
        }

        href += strlen("<d:href>");

        char *href_end = strstr(href, "</d:href>");
        if (!href_end || href_end > resp_end) {
            resp = resp_end + 1;
            continue;
        }

        char full_path[256];
        int len = href_end - href;

        if (len >= sizeof(full_path))
            len = sizeof(full_path) - 1;

        memcpy(full_path, href, len);
        full_path[len] = '\0';

        // Extract filename
        char *name = strrchr(full_path, '/');
        if (name && *(name + 1) != '\0')
        {
            strncpy(remote_files[remote_count], name + 1, MAX_FILENAME - 1);
            remote_files[remote_count][MAX_FILENAME - 1] = '\0';

            ESP_LOGI(TAG, "Remote file: %s", remote_files[remote_count]);

            remote_count++;
        }

        resp = resp_end + 1;
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

static void url_encode(char *out, size_t max_len, const char *in)
{
    const char *hex = "0123456789ABCDEF";
    size_t out_len = 0;

    while (*in && out_len < max_len - 1) {
        unsigned char c = *in;

        if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') {
            *out++ = c;
            out_len++;
        } else if (out_len < max_len - 3) {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 15];
            out_len += 3;
        } else {
            break; // not enough space
        }
        in++;
    }

    *out = '\0';
}

// Download file via microservice
static bool download_file(const char *filename)
{
    char *url = malloc(4096);
    char *path = malloc(256);
    char *buffer = malloc(1024);

    if (!url || !path || !buffer) {
        ESP_LOGE(TAG, "Memory allocation failed");
        free(url);
        free(path);
        free(buffer);
        return false;
    }

    char bmp_name[MAX_FILENAME];
    to_bmp_name(bmp_name, filename);

    // Build encoded remote URL
    char remote_url[512];
    char encoded_url[2048];

    snprintf(remote_url, sizeof(remote_url), "%s%s", WEBDAV_URL, filename);
    url_encode(encoded_url, sizeof(encoded_url), remote_url);

    // Final converter request
    snprintf(url, 4096,
        "%s%s",
        CONVERT_URL,
        encoded_url);

    ESP_LOGI(TAG, "Downloading: %s", filename);
    ESP_LOGI(TAG, "URL: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // ---- multipart body ----
    const char *boundary = "----esp32boundary";

    char body[256];
    snprintf(body, sizeof(body),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "\r\n"
        "--%s--\r\n",
        boundary, boundary);

    char content_type[64];
    snprintf(content_type, sizeof(content_type),
        "multipart/form-data; boundary=%s",
        boundary);

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, body, strlen(body));

    if (esp_http_client_perform(client) != ESP_OK) {
        ESP_LOGE(TAG, "Download failed");
        esp_http_client_cleanup(client);
        goto fail;
    }

    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP status: %d", status);
    ESP_LOGI(TAG, "Content-Length: %lld",
             esp_http_client_get_content_length(client));

    if (status != 200) {
        char errbuf[128];
        int r = esp_http_client_read(client, errbuf, sizeof(errbuf) - 1);
        if (r > 0) {
            errbuf[r] = 0;
            ESP_LOGE(TAG, "Server reply: %s", errbuf);
        }
        esp_http_client_cleanup(client);
        goto fail;
    }

    snprintf(path, 256, "%s%s", STORAGE_PATH, bmp_name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "File open failed");
        esp_http_client_cleanup(client);
        goto fail;
    }

    int read_len;

    while ((read_len = esp_http_client_read(client, buffer, 1024)) > 0) {
        fwrite(buffer, 1, read_len, f);
    }

    fclose(f);
    esp_http_client_cleanup(client);

    free(url);
    free(path);
    free(buffer);

    return true;

fail:
    free(url);
    free(path);
    free(buffer);
    return false;
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