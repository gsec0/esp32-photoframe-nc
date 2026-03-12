#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MAX_REMOTE_FILES 256
#define MAX_FILENAME 128
#define MAX_LOCAL_FILES 256
#define STORAGE_PATH "/sdcard/pictures/"

extern int remote_file_count;
extern char remote_files[MAX_REMOTE_FILES][MAX_FILENAME];

// read_local_files fills local_files and sets *local_count
void sync_read_local_files(char local_files[][MAX_FILENAME], int *local_count);

// delete any local files not present in remote_files
void sync_delete_extra_local_files(char remote_files[][MAX_FILENAME], int remote_count,
                                   char local_files[][MAX_FILENAME], int local_count);

// enqueue downloads for any remote files not present locally
// Uses your global download_queue and download_job_t (declared in system.h)
void sync_enqueue_missing_files(char remote_files[][MAX_FILENAME], int remote_count,
                                char local_files[][MAX_FILENAME], int local_count);

// Fetches remote file list via WebDAV PROPFIND and populates remote_files and remote_file_count
void sync_task(void *arg);

bool fetch_remote_file_list(void);

void to_bmp_name(char *dst, const char *remote_name);