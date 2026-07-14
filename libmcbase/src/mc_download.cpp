#include "mc_download.h"
#include "mc_http.h"
#include "mc_hash.h"
#include "mc_path.h"
#include "mc_str.h"
#include "mc_log.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <chrono>

#include <QtCore/QFile>

#define MIRROR_TEST_URL "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

void mc_downloader_init(McDownloader *dl) {
    memset(dl, 0, sizeof(McDownloader));
    dl->max_retries = 3;
    dl->timeout_ms = 60000;
    dl->verify_hash = 1;
}

void mc_downloader_add_mirror(McDownloader *dl, const char *url) {
    if (!dl || !url || dl->mirror_count >= MC_DL_MAX_MIRRORS) return;
    strncpy(dl->mirrors[dl->mirror_count], url, sizeof(dl->mirrors[0]) - 1);
    dl->mirror_count++;
}

static int try_download_url(McDownloader *dl, const char *url, const char *output_path,
                            const char *expected_sha1, long expected_size,
                            long *downloaded)
{
    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, dl->timeout_ms);
    mc_info("  Downloading: %s", url);
    McHttpResponse *resp = mc_http_get(&client, url);
    if (!resp || !resp->success || !resp->data) {
        if (resp) mc_http_response_free(resp);
        return 0;
    }
    if (resp->status_code >= 400) {
        mc_warn("  HTTP %ld for %s", resp->status_code, url);
        mc_http_response_free(resp);
        return 0;
    }
    // Check size
    if (expected_size > 0 && (long)resp->data_len != expected_size) {
        mc_warn("  Size mismatch: expected %ld, got %zu", expected_size, resp->data_len);
        mc_http_response_free(resp);
        return 0;
    }
    // Write to file
    char dir[MC_PATH_MAX];
    mc_path_dirname(output_path, dir, sizeof(dir));
    mc_path_mkdir_p(dir);
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        mc_warn("  Cannot write to %s", output_path);
        mc_http_response_free(resp);
        return 0;
    }
    fwrite(resp->data, 1, resp->data_len, f);
    fclose(f);
    if (downloaded) *downloaded = (long)resp->data_len;
    // Verify SHA1
    if (dl->verify_hash && expected_sha1 && *expected_sha1) {
        char actual_sha1[64];
        if (mc_hash_file_sha1(output_path, actual_sha1, sizeof(actual_sha1))) {
            if (mc_stricmp(actual_sha1, expected_sha1) != 0) {
                mc_warn("  SHA1 mismatch: expected %s, got %s", expected_sha1, actual_sha1);
                QFile::remove(QString::fromUtf8(output_path));
                mc_http_response_free(resp);
                return 0;
            }
        }
    }
    mc_info("  OK (%ld bytes)", (long)resp->data_len);
    mc_http_response_free(resp);
    return 1;
}

int mc_download_file(McDownloader *dl, McDownloadTask *task, McDownloadResult *result) {
    if (!dl || !task || !result) return 0;
    memset(result, 0, sizeof(McDownloadResult));
    // Try main URL first
    if (task->url[0]) {
        if (try_download_url(dl, task->url, task->output_path,
                            task->expected_sha1, task->expected_size,
                            &result->downloaded_bytes))
        {
            result->success = 1;
            return 1;
        }
    }
    // Try mirrors
    for (int i = 0; i < dl->mirror_count; i++) {
        if (dl->mirrors[i][0]) {
            mc_info("  Trying mirror %d: %s", i + 1, dl->mirrors[i]);
            if (try_download_url(dl, dl->mirrors[i], task->output_path,
                                task->expected_sha1, task->expected_size,
                                &result->downloaded_bytes))
            {
                result->success = 1;
                return 1;
            }
        }
    }
    {
        std::ostringstream oss;
        oss << "Failed to download " << task->url << " after all attempts";
        strncpy(result->error, oss.str().c_str(), sizeof(result->error) - 1);
        result->error[sizeof(result->error) - 1] = '\0';
    }
    mc_error("Download failed: %s", task->url);
    return 0;
}

int mc_download_url(const char *url, const char *output_path, McDownloadResult *result) {
    McDownloader dl;
    mc_downloader_init(&dl);
    dl.verify_hash = 0;
    McDownloadTask task;
    memset(&task, 0, sizeof(task));
    strncpy(task.url, url, sizeof(task.url) - 1);
    if (output_path)
        strncpy(task.output_path, output_path, sizeof(task.output_path) - 1);
    return mc_download_file(&dl, &task, result);
}

int mc_download_translate_mojang_url(const char *url, char *mirror, size_t mirror_size, const char *mirror_type) {
    if (!url || !mirror || mirror_size == 0) return 0;
    // Default mirror type
    if (!mirror_type || strcmp(mirror_type, "mojang") == 0 || strcmp(mirror_type, "auto") == 0) {
        strncpy(mirror, url, mirror_size - 1);
        return 1;
    }

    const char *from_domain = NULL;
    const char *to_domain = NULL;
    int replace_all = 0;

    if (strcmp(mirror_type, "bmclapi") == 0) {
        if (strstr(url, "libraries.minecraft.net")) {
            from_domain = "libraries.minecraft.net";
            to_domain = "bmclapi2.bangbang93.com/maven";
        } else if (strstr(url, "resources.download.minecraft.net")) {
            from_domain = "resources.download.minecraft.net";
            to_domain = "bmclapi2.bangbang93.com/assets";
        } else if (strstr(url, "piston-data.mojang.com") || strstr(url, "piston-meta.mojang.com") ||
                   strstr(url, "launcher.mojang.com") || strstr(url, "launchermeta.mojang.com")) {
            replace_all = 1;
            from_domain = ".mojang.com";
            to_domain = "bmclapi2.bangbang93.com";
        } else if (strstr(url, "maven.fabricmc.net")) {
            from_domain = "maven.fabricmc.net";
            to_domain = "bmclapi2.bangbang93.com/maven";
        } else if (strstr(url, "maven.minecraftforge.net")) {
            from_domain = "maven.minecraftforge.net";
            to_domain = "bmclapi2.bangbang93.com/maven";
        } else if (strstr(url, "maven.neoforged.net")) {
            from_domain = "maven.neoforged.net";
            to_domain = "bmclapi2.bangbang93.com/maven";
        } else {
            return 0;
        }
    } else if (strcmp(mirror_type, "mcbbs") == 0) {
        if (strstr(url, "libraries.minecraft.net") || strstr(url, "maven.fabricmc.net") ||
            strstr(url, "maven.minecraftforge.net") || strstr(url, "maven.neoforged.net")) {
            from_domain = ".net";
            to_domain = "download.mcbbs.net/maven";
            char *extracted = strstr(url, "://");
            if (extracted) extracted += 3; else extracted = (char*)url;
            const char *slash = strchr(extracted, '/');
            if (slash) {
                char path_only[1024];
                strncpy(path_only, slash, sizeof(path_only) - 1);
                char full_url[1024];
                {
                    std::ostringstream oss;
                    oss << "https://download.mcbbs.net/maven" << path_only;
                    strncpy(full_url, oss.str().c_str(), sizeof(full_url) - 1);
                    full_url[sizeof(full_url) - 1] = '\0';
                }
                strncpy(mirror, full_url, mirror_size - 1);
                return 1;
            }
            return 0;
        } else if (strstr(url, "resources.download.minecraft.net")) {
            char *extracted = strstr(url, "://");
            if (extracted) extracted += 3; else extracted = (char*)url;
            const char *slash = strchr(extracted, '/');
            if (slash) {
                char path_only[1024];
                strncpy(path_only, slash, sizeof(path_only) - 1);
                char full_url[1024];
                {
                    std::ostringstream oss;
                    oss << "https://download.mcbbs.net/assets" << path_only;
                    strncpy(full_url, oss.str().c_str(), sizeof(full_url) - 1);
                    full_url[sizeof(full_url) - 1] = '\0';
                }
                strncpy(mirror, full_url, mirror_size - 1);
                return 1;
            }
            return 0;
        } else {
            replace_all = 1;
            from_domain = ".mojang.com";
            to_domain = "download.mcbbs.net";
        }
    } else if (strncmp(mirror_type, "http", 4) == 0) {
        // Custom mirror URL — pass through as-is
        strncpy(mirror, mirror_type, mirror_size - 1);
        return 1;
    } else {
        return 0;
    }

    if (replace_all) {
        // Replace each known Mojang subdomain with the mirror domain
        struct { const char *from; const char *to; } subs[] = {
            { "piston-data.mojang.com", to_domain },
            { "piston-meta.mojang.com", to_domain },
            { "launcher.mojang.com", to_domain },
            { "launchermeta.mojang.com", to_domain },
            { NULL, NULL }
        };
        char *cur = mc_strdup(url);
        if (!cur) return 0;
        for (int i = 0; subs[i].from; i++) {
            if (strstr(cur, subs[i].from)) {
                char *tmp = mc_strreplace(cur, subs[i].from, to_domain);
                free(cur);
                cur = tmp;
                break;
            }
        }
        if (!cur) return 0;
        strncpy(mirror, cur, mirror_size - 1);
        free(cur);
        return 1;
    }

    char *replaced = mc_strreplace(url, from_domain, to_domain);
    if (!replaced) return 0;
    strncpy(mirror, replaced, mirror_size - 1);
    free(replaced);
    return 1;
}

const char *mc_mirror_get_test_url(void) {
    return MIRROR_TEST_URL;
}

int mc_mirror_probe(const char *mirror_type, McMirrorProbe *result) {
    if (!result) return 0;
    memset(result, 0, sizeof(*result));
    if (mirror_type)
        strncpy(result->mirror_type, mirror_type, sizeof(result->mirror_type) - 1);
    else
        strncpy(result->mirror_type, "mojang", sizeof(result->mirror_type) - 1);

    char test_url[2048];
    if (!mc_download_translate_mojang_url(MIRROR_TEST_URL, test_url, sizeof(test_url),
                                          result->mirror_type))
        return 0;

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, MC_MIRROR_PROBE_TIMEOUT_MS);

    auto t1 = std::chrono::steady_clock::now();
    McHttpResponse *resp = mc_http_head(&client, test_url);
    auto t2 = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    result->latency_ms = (double)ms;

    if (resp) {
        result->status_code = (int)resp->status_code;
        result->available = (resp->success && resp->status_code >= 200 && resp->status_code < 400) ? 1 : 0;
        mc_http_response_free(resp);
    }
    return 1;
}

int mc_mirror_probe_all(McMirrorProbe *results, int max_results) {
    if (!results) return 0;
    const char *known_types[] = { "mojang", "bmclapi", "mcbbs", NULL };
    int count = 0;
    for (int i = 0; known_types[i] && count < max_results; i++)
        if (mc_mirror_probe(known_types[i], &results[count]))
            count++;
    return count;
}

const char *mc_mirror_select_best(McMirrorProbe *results, int count) {
    if (!results || count <= 0) return "mojang";
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (!results[i].available) continue;
        if (best < 0 || results[i].latency_ms < results[best].latency_ms)
            best = i;
    }
    if (best >= 0) return results[best].mirror_type;
    // No mirror available, fall back to mojang
    for (int i = 0; i < count; i++) {
        if (strcmp(results[i].mirror_type, "mojang") == 0)
            return "mojang";
    }
    return "mojang";
}
