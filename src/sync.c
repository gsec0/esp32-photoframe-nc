#include "sync.h"
#include "system.h"    // for download_queue, download_job_t
#include "esp_log.h"
#include "esp_http_client.h"

#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define WEBDAV_URL "https://192.168.1.63:7070/remote.php/dav/photospublic/yJ4UmAGcRkslSl2XoQ0MR2mt3n8PacX3/"
#define JPG_EXT  ".jpg"
#define JPEG_EXT ".jpeg"
#define BMP_EXT  ".bmp"

static const char *TAG = "SYNC";
static const char *propfind_body =
"<?xml version=\"1.0\"?>"
"<d:propfind xmlns:d=\"DAV:\">"
"<d:prop>"
"<d:resourcetype/>"
"</d:prop>"
"</d:propfind>";
static int overlap_len = 0;
static char overlap_buf[512];

char remote_files[MAX_REMOTE_FILES][MAX_FILENAME] = {0};
int remote_file_count = 0;

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

bool fetch_remote_file_list(void)
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

static inline bool filename_ends_with(const char *name, const char *suffix)
{
    size_t n = strlen(name);
    size_t s = strlen(suffix);
    if (n < s) return false;
    return (strcmp(name + n - s, suffix) == 0);
}

void to_bmp_name(char *dst, const char *remote_name)
{
    // remote_name is something like "1730627-IMG_20251231_163217.jpg"
    size_t len = strlen(remote_name);

    // Start by copying the whole name (bounded + always null‑terminated)
    strncpy(dst, remote_name, MAX_FILENAME - 1);
    dst[MAX_FILENAME - 1] = '\0';

    // Replace .jpg / .jpeg with .bmp
    if (filename_ends_with(dst, JPG_EXT)) {
        dst[len - strlen(JPG_EXT)] = '\0';
    } else if (filename_ends_with(dst, JPEG_EXT)) {
        dst[len - strlen(JPEG_EXT)] = '\0';
    }

    // Append .bmp if not already there (or after we trimmed jpg/jpeg)
    size_t dlen = strlen(dst);
    if (dlen + strlen(BMP_EXT) < MAX_FILENAME) {
        strcat(dst, BMP_EXT);
    } else {
        // Truncate carefully if MAX_FILENAME is very small
        size_t room = MAX_FILENAME - 1;
        if (room > strlen(BMP_EXT)) {
            room -= strlen(BMP_EXT);
            dst[room] = '\0';
            strcat(dst, BMP_EXT);
        } else {
            // Worst case: just ensure null‑termination, name will be clipped
            dst[MAX_FILENAME - 1] = '\0';
        }
    }
}

bool file_exists_in_list(const char *name, char list[][MAX_FILENAME], int count)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(name, list[i]) == 0) return true;
    }
    return false;
}

void sync_read_local_files(char local_files[][MAX_FILENAME], int *local_count)
{
    if (!local_count) return;
    *local_count = 0;

    DIR *d = opendir(STORAGE_PATH);
    if (!d) {
        ESP_LOGE(TAG, "opendir('%s') failed: %s", STORAGE_PATH, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *local_count < MAX_LOCAL_FILES) {
        // Some filesystems may not fill d_type; accept anything that looks like a file
        if (entry->d_type == DT_DIR) continue;

        // Skip partial downloads and non-bmp files
        if (filename_ends_with(entry->d_name, ".part")) continue;
        if (!filename_ends_with(entry->d_name, ".bmp")) continue;

        strncpy(local_files[*local_count], entry->d_name, MAX_FILENAME - 1);
        local_files[*local_count][MAX_FILENAME - 1] = '\0';
        (*local_count)++;
    }

    closedir(d);

    ESP_LOGI(TAG, "Found %d local files", *local_count);
}

void sync_delete_extra_local_files(char remote_files[][MAX_FILENAME], int remote_count, char local_files[][MAX_FILENAME], int local_count)
{
    char expected_bmp[MAX_FILENAME];
    char path[512];

    for (int i = 0; i < local_count; ++i) {
        bool keep = false;

        for (int j = 0; j < remote_count; ++j) {
            // convert remote filename (jpg) -> bmp name
            // assume you have to_bmp_name() implemented somewhere; if not, implement
            to_bmp_name(expected_bmp, remote_files[j]);
            if (strcmp(local_files[i], expected_bmp) == 0) {
                keep = true;
                break;
            }
        }

        if (!keep) {
            snprintf(path, sizeof(path), "%s%s", STORAGE_PATH, local_files[i]);
            ESP_LOGI(TAG, "Deleting extra local file: %s", path);
            int rc = unlink(path);
            if (rc != 0) {
                ESP_LOGW(TAG, "Failed to delete %s (errno=%d)", path, errno);
            }
        }
    }
}

void sync_enqueue_missing_files(char remote_files[][MAX_FILENAME], int remote_count, char local_files[][MAX_FILENAME], int local_count)
{
    char bmp_name[MAX_FILENAME];
    download_job_t job;

    for (int i = 0; i < remote_count; ++i) {
        to_bmp_name(bmp_name, remote_files[i]);

        if (!file_exists_in_list(bmp_name, local_files, local_count)) {
            // prepare job: file to request from microservice is the remote filename (jpg)
            memset(&job, 0, sizeof(job));
            strncpy(job.filename, remote_files[i], sizeof(job.filename) - 1);

            BaseType_t ok = xQueueSendToBack(download_queue, &job, pdMS_TO_TICKS(1000));
            if (ok != pdTRUE) {
                ESP_LOGW(TAG, "Download queue full; failed to enqueue %s", job.filename);
                // Continue; we might want to retry in a subsequent sync pass
            } else {
                ESP_LOGI(TAG, "Enqueued download: %s", job.filename);
            }
        } else {
            ESP_LOGD(TAG, "Already present locally: %s", bmp_name);
        }
    }
}

// Replace existing sync_task in sync.c with this integrated version

void sync_task(void *arg)
{
    const TickType_t sync_delay = pdMS_TO_TICKS(30000); // 30s for testing
    const int max_enqueue_per_cycle = 8; // safety cap: how many downloads to enqueue per sync

    ESP_LOGI("SYNC", "Sync task started");

    // Wait until WiFi + SD are available
    xEventGroupWaitBits(
        system_events,
        WIFI_CONNECTED_BIT | SD_READY_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI("SYNC", "System ready");

    while (true) {
        ESP_LOGI("SYNC", "Starting sync pass; free heap before: %u", esp_get_free_heap_size());

        // 1) Fetch remote file list (populates remote_files[] and remote_file_count)
        remote_file_count = 0;    // ensure fresh
        overlap_len = 0;         // clear XML overlap state if you use it
        if (!fetch_remote_file_list()) {
            ESP_LOGW("SYNC", "fetch_remote_file_list() failed; skipping this pass");
            vTaskDelay(sync_delay);
            continue;
        }

        ESP_LOGI("SYNC", "Remote files: %d", remote_file_count);

        // 2) Read local files into a local array
        char local_files[MAX_LOCAL_FILES][MAX_FILENAME];
        int local_count = 0;
        sync_read_local_files(local_files, &local_count);
        ESP_LOGI("SYNC", "Local files: %d", local_count);

        // 3) Delete extras
        sync_delete_extra_local_files(remote_files, remote_file_count, local_files, local_count);

        // 4) Enqueue missing files (rate-limited)
        {
            int enqueued = 0;
            char bmp_name[MAX_FILENAME];
            download_job_t job;

            for (int i = 0; i < remote_file_count && enqueued < max_enqueue_per_cycle; ++i) {
                // Build bmp name for comparison
                to_bmp_name(bmp_name, remote_files[i]);

                if (!file_exists_in_list(bmp_name, local_files, local_count)) {
                    // prepare job
                    memset(&job, 0, sizeof(job));
                    strncpy(job.filename, remote_files[i], sizeof(job.filename) - 1);

                    if (xQueueSendToBack(download_queue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
                        ESP_LOGI("SYNC", "Enqueued download: %s", job.filename);
                        ++enqueued;
                    } else {
                        ESP_LOGW("SYNC", "Download queue full; stop enqueueing this pass");
                        break;
                    }
                }
            }
            if (enqueued == 0) ESP_LOGI("SYNC", "No downloads needed this pass");
            else ESP_LOGI("SYNC", "Enqueued %d downloads this pass", enqueued);
        }

        ESP_LOGI("SYNC", "Sync pass complete; free heap after: %u", esp_get_free_heap_size());

        // Delay until next poll (or wait for an event-driven trigger in future)
        vTaskDelay(sync_delay);
    }
}