#include "mc_download_qt.h"
#include "mc_hash.h"
#include "mc_path.h"
#include "mc_str.h"
#include "mc_log.h"
#include <QtCore/QCoreApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QHttp1Configuration>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QThread>
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QHostInfo>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <queue>
#include <future>

static std::vector<QNetworkAccessManager*> g_nam_pool;
static std::atomic<int> g_nam_sel{0};

void mc_qt_download_init(void) {
    int n = 16; // number of QNAMs in pool
    g_nam_pool.reserve(n);
    for (int i = 0; i < n; i++)
        g_nam_pool.push_back(new QNetworkAccessManager());
}
void mc_qt_download_cleanup(void) {
    for (auto *p : g_nam_pool) { delete p; }
    g_nam_pool.clear();
}
static QNetworkAccessManager *pick_nam(void) {
    return g_nam_pool[g_nam_sel++ % (int)g_nam_pool.size()];
}

static void setup_req(QNetworkRequest &req, long timeout_ms) {
    req.setTransferTimeout((int)timeout_ms);
    req.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    req.setRawHeader("Accept", "*/*");
    req.setRawHeader("Accept-Encoding", "gzip, deflate");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QHttp1Configuration h1;
    h1.setNumberOfConnectionsPerHost(64);
    req.setHttp1Configuration(h1);
}

void mc_qt_dns_prefetch(void) {
    std::thread t([]() {
        const char *hosts[] = {
            "piston-meta.mojang.com","piston-data.mojang.com","launcher.mojang.com",
            "launchermeta.mojang.com","resources.download.minecraft.net","libraries.minecraft.net",
            "maven.fabricmc.net","maven.minecraftforge.net","maven.neoforged.net",
            "bmclapi2.bangbang93.com","api.modrinth.com","api.curseforge.com",NULL
        };
        for (int i = 0; hosts[i]; i++) QHostInfo::fromName(QString::fromUtf8(hosts[i]));
    });
    t.detach();
}

// ---- Temp file helpers ----

static void clean_temp(const char *path) {
    for (int i = 0; i < 200; i++) {
        char p[2048]; snprintf(p,sizeof(p),"%s.chunk.%d",path,i);
        QFile::remove(QString::fromUtf8(p));
    }
    char t[2048]; snprintf(t,sizeof(t),"%s%s",path,".PCLDownloading");
    QFile::remove(QString::fromUtf8(t));
}

static int append_to_temp(const char *path, int idx, const QByteArray &data) {
    char p[2048]; snprintf(p,sizeof(p),"%s.chunk.%d",path,idx);
    FILE *f = fopen(p,"wb");
    if (!f) return 0;
    fwrite(data.constData(),1,(size_t)data.size(),f);
    fclose(f);
    return 1;
}

static int merge_file(const char *out, long long expect_size, const char *expect_sha1) {
    int n = 0;
    for (int i = 0; i < 200; i++) {
        char p[2048]; snprintf(p,sizeof(p),"%s.chunk.%d",out,i);
        if (mc_path_exists(p)) n++; else break;
    }
    if (n == 0) return 0;

    char tmp[2048]; snprintf(tmp,sizeof(tmp),"%s%s",out,".PCLDownloading");
    FILE *fo = fopen(tmp,"wb");
    if (!fo) { clean_temp(out); return 0; }
    long long written = 0;
    for (int i = 0; i < n; i++) {
        char cp[2048]; snprintf(cp,sizeof(cp),"%s.chunk.%d",out,i);
        FILE *fi = fopen(cp,"rb");
        if (!fi) { fclose(fo); clean_temp(out); return 0; }
        fseek(fi,0,SEEK_END); long long sz = ftell(fi); fseek(fi,0,SEEK_SET);
        char buf[65536];
        while (sz > 0) {
            long long rd = sz > (long long)sizeof(buf) ? (long long)sizeof(buf) : sz;
            size_t nr = fread(buf,1,(size_t)rd,fi);
            if (nr == 0) break;
            fwrite(buf,1,nr,fo); written += (long long)nr; sz -= (long long)nr;
        }
        fclose(fi); QFile::remove(QString::fromUtf8(cp));
    }
    fclose(fo);

    if (expect_size > 0 && written != expect_size) {
        QFile::remove(QString::fromUtf8(tmp)); clean_temp(out); return 0;
    }
    if (expect_sha1 && *expect_sha1) {
        char sha[64];
        if (mc_hash_file_sha1(tmp,sha,sizeof(sha)) && mc_stricmp(sha,expect_sha1) != 0) {
            QFile::remove(QString::fromUtf8(tmp)); clean_temp(out); return 0;
        }
    }
    if (QFile::rename(QString::fromUtf8(tmp),QString::fromUtf8(out))) return 1;
    FILE *sf = fopen(tmp,"rb"), *df = fopen(out,"wb");
    if (sf && df) {
        char buf[65536]; size_t nr;
        while ((nr = fread(buf,1,sizeof(buf),sf)) > 0) fwrite(buf,1,nr,df);
        fclose(sf); fclose(df); QFile::remove(QString::fromUtf8(tmp)); return 1;
    }
    clean_temp(out); return 0;
}

// ---- Async batch download engine ----
// Pool threads submit tasks via promise/future. Main thread processes
// all pending tasks in parallel on shared g_nam (HTTP/2 multiplexing).

struct DlFile {
    std::string url, path, sha1;
    long size, timeout;
    std::promise<int> promise;
};

static std::mutex g_mtx;
static std::queue<DlFile> g_q;
static std::atomic<bool> g_busy{false};

// Called on main thread via QueuedConnection
static void process_all();

static int submit(const char *url, const char *path,
                   const char *sha1, long size, long timeout)
{
    DlFile f;
    f.url = url ? url : "";
    f.path = path ? path : "";
    f.sha1 = sha1 ? sha1 : "";
    f.size = size;
    f.timeout = timeout;
    auto ft = f.promise.get_future();

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_q.push(std::move(f));
    }

    bool e = false;
    if (g_busy.compare_exchange_strong(e, true))
        QMetaObject::invokeMethod(qApp, process_all, Qt::QueuedConnection);

    return ft.get();
}

// Download a single file using the shared QNAM.
// For small files: streaming single GET -> verify
// For large files: serial chunk GETs -> merge -> verify
// Returns 1 on success, 0 on failure.
struct DlStream {
    FILE *fp{nullptr};
    long long written{0};
};
static int dl_one(const char *url, const char *path,
                   const char *sha1, long size, long timeout)
{
    if (!url || !*url || !path) return 0;
    clean_temp(path);

    const long long THRESH = 1024LL * 1024;
    const int MAX_CHUNKS = 4;
    int nchunks = 1;
    if (size > THRESH) {
        nchunks = std::min(MAX_CHUNKS, (int)(size / THRESH) + 1);
        if (nchunks < 2) nchunks = 2;
    }

    for (int att = 0; att <= 2; att++) {
        if (att > 0) {
            int b = 500 * (1 << (att - 1));
            if (b > 5000) b = 5000;
            QThread::msleep(b);
        }

        if (nchunks > 1) {
            clean_temp(path);
            long long csize = size / nchunks;
            int ok_all = 1;
            for (int i = 0; i < nchunks; i++) {
                long long st = (long long)i * csize;
                long long en = (i == nchunks - 1) ? (size - 1) : (st + csize - 1);
                int chunk_ok = 0;
                auto stream = std::make_shared<DlStream>();
                char cp[2048]; snprintf(cp,sizeof(cp),"%s.chunk.%d",path,i);

                QUrl qurl(QString::fromUtf8(url));
                QNetworkRequest req(qurl);
                setup_req(req, timeout);
                req.setRawHeader("Range", QString("bytes=%1-%2").arg(st).arg(en).toUtf8());
                QNetworkReply *reply = pick_nam()->get(req);
                QEventLoop loop;
                QTimer timer;
                auto timed_out = std::make_shared<bool>(false);
                if (timeout > 0) {
                    timer.setSingleShot(true);
                    QObject::connect(&timer, &QTimer::timeout, [&loop, timed_out]() {
                        *timed_out = true; loop.quit();
                    });
                    timer.start((int)timeout);
                }

                FILE *fp = fopen(cp, "wb");
                if (fp) {
                    stream->fp = fp;
                    QObject::connect(reply, &QNetworkReply::readyRead, [reply, stream]() {
                        QByteArray data = reply->readAll();
                        fwrite(data.constData(), 1, (size_t)data.size(), stream->fp);
                        stream->written += (long long)data.size();
                    });
                }
                QObject::connect(reply, &QNetworkReply::finished, [reply, stream, &loop, &chunk_ok, en, st, timed_out, fp, cp]() {
                    if (fp) {
                        if (!*timed_out && reply->error() == QNetworkReply::NoError) {
                            QByteArray data = reply->readAll();
                            if (!data.isEmpty()) {
                                fwrite(data.constData(), 1, (size_t)data.size(), stream->fp);
                                stream->written += (long long)data.size();
                            }
                            if (stream->written == (en - st + 1))
                                chunk_ok = 1;
                        }
                        fclose(stream->fp); stream->fp = nullptr;
                        if (!chunk_ok) QFile::remove(QString::fromUtf8(cp));
                    }
                    reply->deleteLater();
                    loop.quit();
                });
                loop.exec();
                if (!chunk_ok) { ok_all = 0; break; }
            }
            if (ok_all && merge_file(path, size, sha1))
                return 1;
            clean_temp(path);
            continue;
        }

        // Single chunk: stream to file directly
        char dir[1024];
        mc_path_dirname(path, dir, sizeof(dir));
        mc_path_mkdir_p(dir);
        FILE *fp = fopen(path, "wb");
        if (!fp) return 0;

        auto stream = std::make_shared<DlStream>();
        stream->fp = fp;
        auto ok_flag = std::make_shared<bool>(false);
        auto timed_out = std::make_shared<bool>(false);

        QUrl qurl(QString::fromUtf8(url));
        QNetworkRequest req(qurl);
        setup_req(req, timeout);
        QNetworkReply *reply = pick_nam()->get(req);
        QEventLoop loop;
        QTimer timer;
        if (timeout > 0) {
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, [&loop, timed_out]() {
                *timed_out = true; loop.quit();
            });
            timer.start((int)timeout);
        }

        QObject::connect(reply, &QNetworkReply::readyRead, [reply, stream]() {
            QByteArray data = reply->readAll();
            fwrite(data.constData(), 1, (size_t)data.size(), stream->fp);
            stream->written += (long long)data.size();
        });
        QObject::connect(reply, &QNetworkReply::finished, [reply, stream, &loop, ok_flag, size, timed_out]() {
            if (!*timed_out && reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                if (!data.isEmpty()) {
                    fwrite(data.constData(), 1, (size_t)data.size(), stream->fp);
                    stream->written += (long long)data.size();
                }
                if (size <= 0 || stream->written == size)
                    *ok_flag = true;
            }
            reply->deleteLater();
            loop.quit();
        });
        loop.exec();

        fclose(stream->fp); stream->fp = nullptr;

        if (*ok_flag && !*timed_out) {
            if (sha1 && *sha1) {
                char as[64];
                if (mc_hash_file_sha1(path, as, sizeof(as)) && mc_stricmp(as, sha1) != 0) {
                    QFile::remove(QString::fromUtf8(path));
                    return 0;
                }
            }
            return 1;
        }
    }
    return 0;
}

struct BatchItem {
    DlFile file;
};

struct BatchCtx {
    std::atomic<int> remaining{0};
    std::atomic<size_t> next_idx{0};
    std::atomic<int> active{0};
    std::vector<std::shared_ptr<BatchItem>> items;
    std::atomic<int> last_log{0};
    std::shared_ptr<QElapsedTimer> t0;
    int max_active = 256;
    QEventLoop loop;
};

static void batch_start_one(std::shared_ptr<BatchCtx> ctx, size_t idx) {
    ctx->active++;
    auto &item = ctx->items[idx];
    clean_temp(item->file.path.c_str());

    QUrl qurl(QString::fromUtf8(item->file.url.c_str()));
    QNetworkRequest req(qurl);
    setup_req(req, item->file.timeout);
    QNetworkReply *reply = pick_nam()->get(req);

    QObject::connect(reply, &QNetworkReply::finished, [reply, ctx, idx]() {
        reply->deleteLater();
        int ok = 0;
        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            long expected = ctx->items[idx]->file.size;
            if (expected <= 0 || (long)data.size() == expected) {
                char dir[1024];
                mc_path_dirname(ctx->items[idx]->file.path.c_str(), dir, sizeof(dir));
                mc_path_mkdir_p(dir);
                FILE *fp = fopen(ctx->items[idx]->file.path.c_str(), "wb");
                if (fp) {
                    fwrite(data.constData(), 1, (size_t)data.size(), fp);
                    fclose(fp);
                    if (!ctx->items[idx]->file.sha1.empty()) {
                        char as[64];
                        if (mc_hash_file_sha1(ctx->items[idx]->file.path.c_str(), as, sizeof(as)) &&
                            mc_stricmp(as, ctx->items[idx]->file.sha1.c_str()) == 0) {
                            ok = 1;
                        } else {
                            QFile::remove(QString::fromUtf8(ctx->items[idx]->file.path.c_str()));
                        }
                    } else {
                        ok = 1;
                    }
                }
            }
        }
        if (!ok)
            QFile::remove(QString::fromUtf8(ctx->items[idx]->file.path.c_str()));
        ctx->items[idx]->file.promise.set_value(ok);

        ctx->active--;
        while (ctx->active < ctx->max_active) {
            size_t n = ctx->next_idx++;
            if (n >= ctx->items.size()) break;
            batch_start_one(ctx, n);
        }

        int done = (int)ctx->items.size() - --ctx->remaining;
        if (done - ctx->last_log >= 500 || ctx->remaining == 0) {
            ctx->last_log = done;
            mc_info("batch %d/%zu done, %d remaining, %lldms",
                done, ctx->items.size(), (int)ctx->remaining, ctx->t0->elapsed());
        }
        if (ctx->remaining == 0)
            ctx->loop.quit();
    });
}

static void process_all() {
    std::queue<DlFile> batch;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        batch.swap(g_q);
    }

    if (batch.empty()) { g_busy = false; return; }

    mc_info("process_all: %zu files in batch", batch.size());

    // All files: big first, continuous pipeline with concurrency limit
    auto ctx = std::make_shared<BatchCtx>();
    ctx->items.reserve(batch.size());
    while (!batch.empty()) {
        auto item = std::make_shared<BatchItem>();
        item->file = std::move(batch.front());
        ctx->items.push_back(std::move(item));
        batch.pop();
    }
    std::sort(ctx->items.begin(), ctx->items.end(), [](const auto &a, const auto &b) {
        return a->file.size > b->file.size;
    });
    ctx->remaining = (int)ctx->items.size();
    ctx->t0 = std::make_shared<QElapsedTimer>();
    ctx->t0->start();

    size_t to_start = std::min((size_t)ctx->max_active, ctx->items.size());
    ctx->next_idx = to_start;
    for (size_t i = 0; i < to_start; i++)
        batch_start_one(ctx, i);

    ctx->loop.exec();

    g_busy = false;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_q.empty()) {
            bool e = false;
            if (g_busy.compare_exchange_strong(e, true))
                QMetaObject::invokeMethod(qApp, process_all, Qt::QueuedConnection);
        }
    }
}

// ---- Public API ----

int mc_qt_download_batch(const char **urls, const char **paths,
                          const char **sha1s, const long *sizes,
                          int count, long timeout_ms,
                          int results[])
{
    if (count <= 0) return 0;
    if (results) memset(results, 0, (size_t)count * sizeof(int));
    std::vector<std::future<int>> futs;
    futs.reserve(count);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (int i = 0; i < count; i++) {
            DlFile f;
            f.url = urls[i] ? urls[i] : "";
            f.path = paths[i] ? paths[i] : "";
            f.sha1 = (sha1s && sha1s[i]) ? sha1s[i] : "";
            f.size = sizes ? sizes[i] : 0L;
            f.timeout = timeout_ms;
            futs.push_back(f.promise.get_future());
            g_q.push(std::move(f));
        }
    }
    bool e = false;
    if (g_busy.compare_exchange_strong(e, true))
        QMetaObject::invokeMethod(qApp, process_all, Qt::QueuedConnection);
    int ok = 0;
    for (int i = 0; i < count; i++) {
        while (futs[i].wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
            if (QCoreApplication::instance())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        int r = futs[i].get();
        if (r) ok++;
        if (results) results[i] = r;
    }
    return ok;
}

int mc_qt_download_file(const char *url, const char *path,
                        const char *sha1, long size, long timeout)
{
    if (QThread::currentThread() == qApp->thread())
        return dl_one(url, path, sha1, size, timeout);
    return submit(url, path, sha1, size, timeout);
}

int mc_qt_download_file_progress(const char *url, const char *path,
                                  const char *sha1, long size,
                                  long timeout,
                                  McDownloadProgressFn progress, void *userdata)
{
    // Progress not supported in async path; fallback to sync on main thread
    if (QThread::currentThread() == qApp->thread())
        return dl_one(url, path, sha1, size, timeout);
    return submit(url, path, sha1, size, timeout);
}

int mc_qt_download_file_multi(const char **urls, int n,
                               const char *path,
                               const char *sha1, long size, long timeout)
{
    if (!urls || n <= 0 || !path) return 0;
    if (n == 1) return mc_qt_download_file(urls[0], path, sha1, size, timeout);
    for (int i = 0; i < n; i++) {
        if (mc_qt_download_file(urls[i], path, sha1, size, timeout)) return 1;
        clean_temp(path);
    }
    return 0;
}
