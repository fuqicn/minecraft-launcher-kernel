/*
 * MIT License
 *
 * Copyright (c) 2026 fuqicn
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MC_DOWNLOAD_QT_H
#define MC_DOWNLOAD_QT_H

#include <stddef.h>

typedef void (*McDownloadProgressFn)(long long received, long long total, void *userdata);

// Initialize global shared QNetworkAccessManager (must be called from main thread).
void mc_qt_download_init(void);
// Cleanup global shared QNetworkAccessManager.
void mc_qt_download_cleanup(void);

// Thread-safe single file download using QtNetwork internals.
// Routes to main thread for shared QNAM connection reuse (PCLCE IHttpClientFactory pattern).
int mc_qt_download_file(const char *url, const char *output_path,
                        const char *expected_sha1, long expected_size,
                        long timeout_ms);

// Variant with progress callback. Called periodically with received/total bytes.
int mc_qt_download_file_progress(const char *url, const char *output_path,
                                  const char *expected_sha1, long expected_size,
                                  long timeout_ms,
                                  McDownloadProgressFn progress, void *userdata);

// Download a file trying multiple URLs in parallel.
int mc_qt_download_file_multi(const char **urls, int url_count,
                               const char *output_path,
                               const char *expected_sha1, long expected_size,
                               long timeout_ms);

// Pre-resolve DNS for common Minecraft hosts.
void mc_qt_dns_prefetch(void);

// Batch submit: enqueue all files at once, wait for all.
// URLs/paths/sizes must have 'count' elements. sha1s may be NULL.
// results (size count) receives 1/0 per file. Returns total success count.
int mc_qt_download_batch(const char **urls, const char **paths,
                          const char **sha1s, const long *sizes,
                          int count, long timeout_ms,
                          int results[]);

#endif
