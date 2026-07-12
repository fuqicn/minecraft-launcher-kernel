#include "mc_manifest.h"
#include "mc_http.h"
#include "mc_json.h"
#include "mc_path.h"
#include "mc_str.h"
#include "mc_log.h"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <sstream>
#include <iostream>

#define MOJANG_MANIFEST_URL "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
#define BMCLAPI_MANIFEST_URL "https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json"
#define MCBBS_MANIFEST_URL "https://download.mcbbs.net/mc/game/version_manifest_v2.json"

const char *mc_manifest_url_for_mirror(const char *mirror_type) {
    if (!mirror_type || strcmp(mirror_type, "mojang") == 0 || strcmp(mirror_type, "auto") == 0)
        return MOJANG_MANIFEST_URL;
    if (strcmp(mirror_type, "bmclapi") == 0)
        return BMCLAPI_MANIFEST_URL;
    if (strcmp(mirror_type, "mcbbs") == 0)
        return MCBBS_MANIFEST_URL;
    return mirror_type;
}

static int manifest_cache_path(char *out, size_t size) {
    char appdir[MC_PATH_MAX];
    if (!mc_path_appdata(appdir, sizeof(appdir))) return 0;
    mc_path_mkdir_p(appdir);
    return mc_path_join(appdir, "version_manifest.json", out, static_cast<int>(size));
}

static long long parse_mojang_time(const char *s) {
    if (!s || !*s) return 0;
    struct tm tm = {};
    int tz_h = 0, tz_m = 0;
    char tz_sign = '+';
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                   &tz_sign, &tz_h, &tz_m);
    if (n < 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = 0;
    long long t = static_cast<long long>(mktime(&tm));
    if (t == -1) return 0;
    if (n >= 7) {
        if (tz_sign == '+') t -= (tz_h * 3600LL + tz_m * 60LL);
        else if (tz_sign == '-') t += (tz_h * 3600LL + tz_m * 60LL);
        else if (tz_sign == 'Z') { }
    }
    return t;
}

static int parse_manifest(const char *data, McManifest *m) {
    McJson *j = mc_json_parse(data);
    if (!j || j->type != MC_JSON_OBJECT) { mc_json_free(j); return 0; }
    const char *lr = mc_json_get_string(j, "latest.release", nullptr);
    if (lr) strncpy(m->latest_release, lr, sizeof(m->latest_release) - 1);
    const char *ls = mc_json_get_string(j, "latest.snapshot", nullptr);
    if (ls) strncpy(m->latest_snapshot, ls, sizeof(m->latest_snapshot) - 1);

    McJson *latest = mc_json_get(j, "latest");
    if (latest) {
        const char *rl = mc_json_get_string(latest, "release", nullptr);
        if (rl) strncpy(m->latest_release, rl, sizeof(m->latest_release) - 1);
        const char *sn = mc_json_get_string(latest, "snapshot", nullptr);
        if (sn) strncpy(m->latest_snapshot, sn, sizeof(m->latest_snapshot) - 1);
    }

    McJson *versions = mc_json_get(j, "versions");
    if (!versions || versions->type != MC_JSON_ARRAY) { mc_json_free(j); return 0; }

    m->count = 0;
    int n = mc_json_array_length(versions);
    if (n > MC_MANIFEST_MAX_VERSIONS) n = MC_MANIFEST_MAX_VERSIONS;

    for (int i = 0; i < n; i++) {
        McJson *v = mc_json_get_array_item(versions, i);
        if (!v) continue;
        McVersionEntry *e = &m->entries[m->count];
        const char *id = mc_json_get_string(v, "id", "");
        strncpy(e->id, id, sizeof(e->id) - 1);
        const char *type = mc_json_get_string(v, "type", "");
        strncpy(e->type, type, sizeof(e->type) - 1);
        const char *url = mc_json_get_string(v, "url", "");
        strncpy(e->url, url, sizeof(e->url) - 1);
        const char *sha1 = mc_json_get_string(v, "sha1", "");
        strncpy(e->sha1, sha1, sizeof(e->sha1) - 1);
        e->release_time = parse_mojang_time(mc_json_get_string(v, "releaseTime", ""));
        e->modified_time = parse_mojang_time(mc_json_get_string(v, "time", ""));
        m->count++;
    }
    m->fetch_time = static_cast<long>(time(nullptr));
    mc_json_free(j);
    return 1;
}

int mc_manifest_fetch(McManifest *m, int force_refresh) {
    return mc_manifest_fetch_mirror(m, force_refresh, nullptr);
}

int mc_manifest_fetch_mirror(McManifest *m, int force_refresh, const char *mirror_type) {
    if (!m) return 0;
    if (!force_refresh && mc_manifest_load_cache(m) && m->count > 0) {
        mc_info("Loaded manifest from cache (%d versions)", m->count);
        return 1;
    }

    const char *primary_url = mc_manifest_url_for_mirror(mirror_type);
    const char *mirrors[] = {
        MOJANG_MANIFEST_URL,
        BMCLAPI_MANIFEST_URL,
        MCBBS_MANIFEST_URL
    };
    int nm = 3;

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 30000);

    int ok = 0;
    mc_info("Fetching manifest: %s", primary_url);
    McHttpResponse *resp = mc_http_get(&client, primary_url);
    if (resp && resp->success && resp->data) {
        ok = parse_manifest(resp->data, m);
        if (ok) mc_info("Fetched manifest: %d versions", m->count);
        else mc_warn("Failed to parse manifest");
    }
    if (resp) mc_http_response_free(resp);

    if (!ok) {
        for (int i = 0; i < nm; i++) {
            if (strcmp(mirrors[i], primary_url) == 0) continue;
            mc_info("Trying mirror: %s", mirrors[i]);
            resp = mc_http_get(&client, mirrors[i]);
            if (resp && resp->success && resp->data) {
                ok = parse_manifest(resp->data, m);
                if (ok) { mc_info("Fetched manifest: %d versions", m->count); break; }
            }
            if (resp) mc_http_response_free(resp);
        }
    }

    if (ok) {
        mc_manifest_save_cache(m);
        return 1;
    }
    mc_error("Failed to fetch version manifest from all sources");
    return 0;
}

int mc_manifest_load_cache(McManifest *m) {
    if (!m) return 0;
    char path[MC_PATH_MAX];
    if (!manifest_cache_path(path, sizeof(path))) return 0;
    if (!mc_path_exists(path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 0; }
    char *data = static_cast<char *>(malloc(static_cast<size_t>(len) + 1));
    if (!data) { fclose(f); return 0; }
    fread(data, 1, static_cast<size_t>(len), f);
    data[len] = '\0';
    fclose(f);
    int ok = parse_manifest(data, m);
    free(data);
    return ok;
}

int mc_manifest_save_cache(const McManifest *m) {
    if (!m || m->count == 0) return 0;
    char path[MC_PATH_MAX];
    if (!manifest_cache_path(path, sizeof(path))) return 0;
    McJson *j = mc_json_create_object();
    if (!j) return 0;
    McJson *latest = mc_json_create_object();
    mc_json_add_string(latest, "release", m->latest_release);
    mc_json_add_string(latest, "snapshot", m->latest_snapshot);
    mc_json_add_item(j, latest);
    if (latest->key) free(latest->key);
    latest->key = mc_strdup("latest");
    McJson *versions = mc_json_create_array();
    for (int i = 0; i < m->count; i++) {
        McJson *v = mc_json_create_object();
        mc_json_add_string(v, "id", m->entries[i].id);
        mc_json_add_string(v, "type", m->entries[i].type);
        mc_json_add_string(v, "url", m->entries[i].url);
        mc_json_add_string(v, "sha1", m->entries[i].sha1);
        time_t rt = static_cast<time_t>(m->entries[i].release_time);
        struct tm *rtm = localtime(&rt);
        if (rtm) { char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", rtm); mc_json_add_string(v, "releaseTime", buf); }
        time_t mt = static_cast<time_t>(m->entries[i].modified_time);
        struct tm *mtm = localtime(&mt);
        if (mtm) { char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", mtm); mc_json_add_string(v, "time", buf); }
        mc_json_add_item(versions, v);
    }
    mc_json_add_item(j, versions);
    if (versions->key) free(versions->key);
    versions->key = mc_strdup("versions");
    char *json_str = mc_json_stringify(j);
    int ok = 0;
    if (json_str) {
        FILE *f = fopen(path, "w");
        if (f) { fputs(json_str, f); fclose(f); ok = 1; }
        free(json_str);
    }
    mc_json_free(j);
    return ok;
}

int mc_manifest_find(const McManifest *m, const char *id, McVersionEntry *out) {
    if (!m || !id || !out) return 0;
    for (int i = 0; i < m->count; i++) {
        if (mc_stricmp(m->entries[i].id, id) == 0) {
            *out = m->entries[i];
            return 1;
        }
    }
    return 0;
}

int mc_manifest_search(const McManifest *m, const char *query, McVersionEntry *results, int max_results) {
    if (!m || !query || !results || max_results <= 0) return 0;
    int found = 0;
    for (int i = 0; i < m->count && found < max_results; i++) {
        if (mc_strwildcard(m->entries[i].id, query) ||
            mc_strcontains(m->entries[i].id, query)) {
            results[found++] = m->entries[i];
        }
    }
    return found;
}
