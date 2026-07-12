#ifndef MC_DOWNLOAD_H
#define MC_DOWNLOAD_H

#include <stddef.h>

#define MC_DL_MAX_MIRRORS 4

typedef struct {
    char url[2048];
    char output_path[1024];
    char expected_sha1[64];
    long expected_size;
    int retry_count;
    int max_retries;
} McDownloadTask;

typedef struct {
    int success;
    char error[512];
    long downloaded_bytes;
} McDownloadResult;

typedef struct {
    char mirrors[MC_DL_MAX_MIRRORS][2048];
    int mirror_count;
    int max_retries;
    int timeout_ms;
    int verify_hash;
} McDownloader;

void mc_downloader_init(McDownloader *dl);
void mc_downloader_add_mirror(McDownloader *dl, const char *url);
int mc_download_file(McDownloader *dl, McDownloadTask *task, McDownloadResult *result);
int mc_download_url(const char *url, const char *output_path, McDownloadResult *result);

// URL mirror translation helpers
int mc_download_translate_mojang_url(const char *url, char *mirror, size_t mirror_size, const char *mirror_type);

#endif
