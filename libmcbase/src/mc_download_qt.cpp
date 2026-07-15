#include "mc_download_qt.h"
#include "mc_hash.h"
#include "mc_path.h"
#include "mc_str.h"
#include "mc_log.h"
#include <QtCore/QCoreApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtNetwork/QHostInfo>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <thread>
#include <atomic>
#include <vector>

static void setup_dl_request(QNetworkRequest &req, long timeout_ms) {
    req.setTransferTimeout(static_cast<int>(timeout_ms));
    req.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    req.setRawHeader("Accept", "*/*");
    req.setRawHeader("Accept-Encoding", "gzip, deflate");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
}

// ---- DNS pre-resolution ----

void mc_qt_dns_prefetch(void) {
    std::thread t([]() {
        static const char *hosts[] = {
            "piston-meta.mojang.com",
            "piston-data.mojang.com",
            "launcher.mojang.com",
            "launchermeta.mojang.com",
            "resources.download.minecraft.net",
            "libraries.minecraft.net",
            "maven.fabricmc.net",
            "maven.minecraftforge.net",
            "maven.neoforged.net",
            "bmclapi2.bangbang93.com",
            "api.modrinth.com",
            "api.curseforge.com",
            NULL
        };
        for (int i = 0; hosts[i]; i++) {
            QHostInfo::fromName(QString::fromUtf8(hosts[i]));
        }
    });
    t.detach();
}

// ---- Chunked download ----

static int do_chunk_download(const char *url, long long start, long long end,
                             const char *output_path, int chunk_idx,
                             long expected_total_size, long timeout_ms)
{
    QNetworkAccessManager nam;
    QUrl qurl(QString::fromUtf8(url));
    QNetworkRequest req(qurl);
    setup_dl_request(req, timeout_ms);
    req.setRawHeader("Range", QString("bytes=%1-%2").arg(start).arg(end).toUtf8());

    QNetworkReply *reply = nam.get(req);

    QEventLoop loop;
    QTimer timer;
    if (timeout_ms > 0) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(static_cast<int>(timeout_ms));
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError) {
        reply->abort();
        delete reply;
        return 0;
    }

    QByteArray data = reply->readAll();
    delete reply;

    if (static_cast<long long>(data.size()) != (end - start + 1))
        return 0;

    char chunk_path[2048];
    snprintf(chunk_path, sizeof(chunk_path), "%s.chunk.%d", output_path, chunk_idx);
    FILE *f = fopen(chunk_path, "wb");
    if (!f) return 0;
    fwrite(data.constData(), 1, static_cast<size_t>(data.size()), f);
    fclose(f);
    return 1;
}

static int cleanup_temp_files(const char *output_path) {
    for (int i = 0; i < 200; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s.chunk.%d", output_path, i);
        QFile::remove(QString::fromUtf8(p));
    }
    char temp[2048];
    snprintf(temp, sizeof(temp), "%s%s", output_path, ".PCLDownloading");
    QFile::remove(QString::fromUtf8(temp));
    return 0;
}

static int merge_and_verify(const char *output_path, long long expected_size,
                             const char *expected_sha1, long long *total_written)
{
    // Collect chunk files in order (sequential indices)
    int chunk_count = 0;
    for (int i = 0; i < 200; i++) {
        char chunk_path[2048];
        snprintf(chunk_path, sizeof(chunk_path), "%s.chunk.%d", output_path, i);
        if (mc_path_exists(chunk_path))
            chunk_count++;
        else
            break; // chunks are sequential, stop at first missing
    }
    if (chunk_count == 0) return 0;

    // Write to temp file with .PCLDownloading extension
    char temp_path[2048];
    snprintf(temp_path, sizeof(temp_path), "%s%s", output_path, ".PCLDownloading");
    FILE *out = fopen(temp_path, "wb");
    if (!out) { cleanup_temp_files(output_path); return 0; }

    long long written = 0;
    for (int i = 0; i < chunk_count; i++) {
        char chunk_path[2048];
        snprintf(chunk_path, sizeof(chunk_path), "%s.chunk.%d", output_path, i);
        FILE *fin = fopen(chunk_path, "rb");
        if (!fin) { fclose(out); cleanup_temp_files(output_path); return 0; }
        fseek(fin, 0, SEEK_END);
        long long size = ftell(fin);
        fseek(fin, 0, SEEK_SET);
        char buf[65536];
        while (size > 0) {
            long long to_read = size > sizeof(buf) ? sizeof(buf) : size;
            size_t n = fread(buf, 1, static_cast<size_t>(to_read), fin);
            if (n == 0) break;
            fwrite(buf, 1, n, out);
            written += n;
            size -= n;
        }
        fclose(fin);
        QFile::remove(QString::fromUtf8(chunk_path));
    }
    fclose(out);

    // Verify size
    if (expected_size > 0 && written != expected_size) {
        QFile::remove(QString::fromUtf8(temp_path));
        cleanup_temp_files(output_path);
        return 0;
    }

    // Verify SHA1
    if (expected_sha1 && *expected_sha1) {
        char actual_sha1[64];
        if (mc_hash_file_sha1(temp_path, actual_sha1, sizeof(actual_sha1))) {
            if (mc_stricmp(actual_sha1, expected_sha1) != 0) {
                QFile::remove(QString::fromUtf8(temp_path));
                cleanup_temp_files(output_path);
                return 0;
            }
        }
    }

    // Rename temp to final
    if (QFile::rename(QString::fromUtf8(temp_path), QString::fromUtf8(output_path))) {
        if (total_written) *total_written = written;
        return 1;
    }
    FILE *sf = fopen(temp_path, "rb");
    FILE *df = fopen(output_path, "wb");
    if (sf && df) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), sf)) > 0)
            fwrite(buf, 1, n, df);
        fclose(sf);
        fclose(df);
        QFile::remove(QString::fromUtf8(temp_path));
        if (total_written) *total_written = written;
        return 1;
    }
    cleanup_temp_files(output_path);
    return 0;
}

// ---- Single file download (no chunking) ----

static int do_single_download(const char *url, const char *output_path,
                               const char *expected_sha1, long expected_size,
                               long timeout_ms)
{
    QNetworkAccessManager nam;
    QUrl qurl(QString::fromUtf8(url));
    QNetworkRequest req(qurl);
    setup_dl_request(req, timeout_ms);

    QNetworkReply *reply = nam.get(req);

    QEventLoop loop;
    QTimer timer;
    if (timeout_ms > 0) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(static_cast<int>(timeout_ms));
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError) {
        reply->abort();
        delete reply;
        return 0;
    }

    QByteArray data = reply->readAll();
    delete reply;

    if (expected_size > 0 && static_cast<long>(data.size()) != expected_size)
        return 0;

    char dir[1024];
    mc_path_dirname(output_path, dir, sizeof(dir));
    mc_path_mkdir_p(dir);
    FILE *f = fopen(output_path, "wb");
    if (!f) return 0;
    fwrite(data.constData(), 1, static_cast<size_t>(data.size()), f);
    fclose(f);

    if (expected_sha1 && *expected_sha1) {
        char actual_sha1[64];
        if (mc_hash_file_sha1(output_path, actual_sha1, sizeof(actual_sha1))) {
            if (mc_stricmp(actual_sha1, expected_sha1) != 0) {
                QFile::remove(QString::fromUtf8(output_path));
                return 0;
            }
        }
    }
    return 1;
}

// ---- Public API ----

int mc_qt_download_file(const char *url, const char *output_path,
                        const char *expected_sha1, long expected_size,
                        long timeout_ms)
{
    return mc_qt_download_file_progress(url, output_path, expected_sha1, expected_size, timeout_ms, nullptr, nullptr);
}

int mc_qt_download_file_progress(const char *url, const char *output_path,
                                  const char *expected_sha1, long expected_size,
                                  long timeout_ms,
                                  McDownloadProgressFn progress, void *userdata)
{
    if (!url || !*url || !output_path) return 0;

    // Clean up any leftover temp files from previous failed downloads
    cleanup_temp_files(output_path);

    // Decide chunked vs single download
    // Threshold: 1MB (same as PCLCE)
    const long long CHUNK_THRESHOLD = 1024LL * 1024;
    const int MAX_CHUNKS = 4;

    int num_chunks = 1;
    if (expected_size > CHUNK_THRESHOLD) {
        num_chunks = std::min(MAX_CHUNKS, static_cast<int>(expected_size / CHUNK_THRESHOLD) + 1);
        if (num_chunks < 2) num_chunks = 2;
    }

    if (num_chunks == 1) {
        // Single download with retry and exponential backoff
        int max_retries = 2;
        for (int attempt = 0; attempt <= max_retries; attempt++) {
            if (attempt > 0) {
                int backoff_ms = 500 * (1 << (attempt - 1));
                if (backoff_ms > 5000) backoff_ms = 5000;
                QThread::msleep(backoff_ms);
            }
            int r = do_single_download(url, output_path, expected_sha1, expected_size, timeout_ms);
            if (r) return 1;
            if (mc_path_exists(output_path)) QFile::remove(QString::fromUtf8(output_path));
        }
        return 0;
    }

    // Chunked download — serial chunks (thread pool already provides file-level parallelism)
    long long chunk_size = expected_size / num_chunks;
    int chunk_ok = 0;
    int chunk_fail = 0;

    for (int i = 0; i < num_chunks; i++) {
        long long start = static_cast<long long>(i) * chunk_size;
        long long end = (i == num_chunks - 1) ? (expected_size - 1) : (start + chunk_size - 1);

        int chunk_success = 0;
        for (int attempt = 0; attempt <= 2; attempt++) {
            if (attempt > 0) {
                int backoff_ms = 500 * (1 << (attempt - 1));
                if (backoff_ms > 5000) backoff_ms = 5000;
                QThread::msleep(backoff_ms);
            }
            if (do_chunk_download(url, start, end, output_path, i, expected_size, timeout_ms)) {
                chunk_success = 1;
                break;
            }
            char chunk_path[2048];
            snprintf(chunk_path, sizeof(chunk_path), "%s.chunk.%d", output_path, i);
            QFile::remove(QString::fromUtf8(chunk_path));
        }
        if (chunk_success) chunk_ok++; else chunk_fail++;

        if (progress) {
            long long total_done = 0;
            for (int j = 0; j <= i; j++) {
                char cp[2048];
                snprintf(cp, sizeof(cp), "%s.chunk.%d", output_path, j);
                if (mc_path_exists(cp)) {
                    FILE *f = fopen(cp, "rb");
                    if (f) { fseek(f, 0, SEEK_END); total_done += ftell(f); fclose(f); }
                }
            }
            progress(total_done, expected_size, userdata);
        }
    }

    if (chunk_fail > 0) {
        cleanup_temp_files(output_path);
        return 0;
    }

    // Merge chunks
    long long total_written = 0;
    int merged = merge_and_verify(output_path, expected_size, expected_sha1, &total_written);
    if (!merged) {
        cleanup_temp_files(output_path);
        return 0;
    }
    return 1;
}

// ---- Multi-URL parallel download ----

int mc_qt_download_file_multi(const char **urls, int url_count,
                               const char *output_path,
                               const char *expected_sha1, long expected_size,
                               long timeout_ms)
{
    if (!urls || url_count <= 0 || !output_path) return 0;

    // Single URL: direct download in calling thread (reuses its QNAM)
    if (url_count == 1)
        return mc_qt_download_file(urls[0], output_path, expected_sha1, expected_size, timeout_ms);

    cleanup_temp_files(output_path);

    std::atomic<int> winner{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < url_count; i++) {
        threads.emplace_back([i, &winner, &urls, output_path, expected_sha1, expected_size, timeout_ms]() {
            if (winner.load()) return;

            char tmp[2048];
            snprintf(tmp, sizeof(tmp), "%s.multi.%d", output_path, i);

            if (do_single_download(urls[i], tmp, expected_sha1, expected_size, timeout_ms)) {
                int expected = 0;
                if (winner.compare_exchange_strong(expected, 1)) {
                    char dir[1024];
                    mc_path_dirname(output_path, dir, sizeof(dir));
                    mc_path_mkdir_p(dir);
                    QFile::remove(QString::fromUtf8(output_path));
                    QFile::rename(QString::fromUtf8(tmp), QString::fromUtf8(output_path));
                } else {
                    QFile::remove(QString::fromUtf8(tmp));
                }
            }
        });
    }
    for (auto &t : threads)
        if (t.joinable()) t.join();

    for (int i = 0; i < url_count; i++) {
        char tmp[2048];
        snprintf(tmp, sizeof(tmp), "%s.multi.%d", output_path, i);
        QFile::remove(QString::fromUtf8(tmp));
    }

    return winner.load();
}
