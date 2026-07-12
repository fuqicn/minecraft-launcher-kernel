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

#endif
