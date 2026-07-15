#ifndef MC_DOWNLOAD_QT_H
#define MC_DOWNLOAD_QT_H

#include <stddef.h>

typedef void (*McDownloadProgressFn)(long long received, long long total, void *userdata);

// Thread-safe single file download using QtNetwork internals.
// Each call creates its own QNetworkAccessManager + event loop.
// Can be called from any thread (QCoreApplication must exist).
int mc_qt_download_file(const char *url, const char *output_path,
                        const char *expected_sha1, long expected_size,
                        long timeout_ms);

// Variant with progress callback. Called periodically with received/total bytes.
// total may be -1 if unknown. userdata is passed through.
int mc_qt_download_file_progress(const char *url, const char *output_path,
                                  const char *expected_sha1, long expected_size,
                                  long timeout_ms,
                                  McDownloadProgressFn progress, void *userdata);

// Download a file trying multiple URLs in parallel.
// Launches up to url_count simultaneous attempts; the first to succeed wins.
// Returns 1 if any URL succeeded, 0 if all failed.
int mc_qt_download_file_multi(const char **urls, int url_count,
                               const char *output_path,
                               const char *expected_sha1, long expected_size,
                               long timeout_ms);

// Pre-resolve DNS for common Minecraft hosts to reduce first-connection latency.
// Non-blocking; lookup runs in a background thread.
void mc_qt_dns_prefetch(void);

#endif
