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

// Mirror health probe
#define MC_MIRROR_PROBE_TIMEOUT_MS 5000
#define MC_MAX_MIRROR_TYPES 8

typedef struct {
    char mirror_type[64];
    double latency_ms;
    int available;
    int status_code;
} McMirrorProbe;

// Probe a specific mirror type. Returns 1 if probe was attempted (result may still show unavailable).
int mc_mirror_probe(const char *mirror_type, McMirrorProbe *result);

// Probe all known mirror types. Returns number of probed types.
int mc_mirror_probe_all(McMirrorProbe *results, int max_results);

// Find the best available mirror from probe results. Returns mirror_type string or "mojang".
const char *mc_mirror_select_best(McMirrorProbe *results, int count);

// Get the test URL used for mirror probing.
const char *mc_mirror_get_test_url(void);

#endif
