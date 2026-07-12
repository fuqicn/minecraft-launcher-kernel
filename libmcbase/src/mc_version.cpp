#include "mc_version.h"
#include "mc_http.h"
#include "mc_json.h"
#include "mc_str.h"
#include "mc_manifest.h"
#include "mc_download.h"
#include "mc_path.h"
#include "mc_log.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <iostream>

void mc_version_init(McVersion *v) {
    memset(v, 0, sizeof(McVersion));
}

static void parse_library(McVersion *v, McJson *lib_json) {
    if (v->library_count >= MC_MAX_LIBRARIES) return;
    McLibrary *lib = &v->libraries[v->library_count];
    memset(lib, 0, sizeof(McLibrary));

    const char *name = mc_json_get_string(lib_json, "name", "");
    strncpy(lib->name, name, sizeof(lib->name) - 1);
    lib->is_required = 1;

    // Parse rules
    McJson *rules = mc_json_get(lib_json, "rules");
    if (rules) {
        lib->is_required = mc_version_evaluate_rules(rules);
        mc_debug("  parse_library '%s': rules -> is_required=%d", name, lib->is_required);
    }

    // Parse downloads
    McJson *downloads = mc_json_get(lib_json, "downloads");
    if (downloads) {
        McJson *artifact = mc_json_get(downloads, "artifact");
        if (artifact) {
            const char *url = mc_json_get_string(artifact, "url", "");
            strncpy(lib->url, url, sizeof(lib->url) - 1);
            const char *path = mc_json_get_string(artifact, "path", "");
            strncpy(lib->path, path, sizeof(lib->path) - 1);
            const char *sha1 = mc_json_get_string(artifact, "sha1", "");
            strncpy(lib->sha1, sha1, sizeof(lib->sha1) - 1);
            lib->size = (long)mc_json_get_number(artifact, "size", 0);
        }

        // Check for natives classifiers
        McJson *classifiers = mc_json_get(downloads, "classifiers");
        if (classifiers && classifiers->type == MC_JSON_OBJECT) {
            // Look for natives-windows classifier
            McJson *natives_win = mc_json_get(classifiers, "natives-windows");
            if (natives_win) {
                lib->is_natives = 1;
                strcpy(lib->natives_key, "natives-windows");
                const char *url = mc_json_get_string(natives_win, "url", "");
                strncpy(lib->url, url, sizeof(lib->url) - 1);
                const char *path = mc_json_get_string(natives_win, "path", "");
                strncpy(lib->path, path, sizeof(lib->path) - 1);
                const char *sha1 = mc_json_get_string(natives_win, "sha1", "");
                strncpy(lib->sha1, sha1, sizeof(lib->sha1) - 1);
                lib->size = (long)mc_json_get_number(natives_win, "size", 0);
            } else {
                // Try with ${arch} replacement
                const char *key_n32 = "natives-windows-32";
                const char *key_n64 = "natives-windows-64";
                McJson *n64 = mc_json_get(classifiers, key_n64);
                if (n64) {
                    lib->is_natives = 1;
                    strcpy(lib->natives_key, "natives-windows-64");
                    const char *url = mc_json_get_string(n64, "url", "");
                    strncpy(lib->url, url, sizeof(lib->url) - 1);
                    const char *path = mc_json_get_string(n64, "path", "");
                    strncpy(lib->path, path, sizeof(lib->path) - 1);
                    const char *sha1 = mc_json_get_string(n64, "sha1", "");
                    strncpy(lib->sha1, sha1, sizeof(lib->sha1) - 1);
                    lib->size = (long)mc_json_get_number(n64, "size", 0);
                }
                if (!lib->is_natives) {
                    McJson *n32 = mc_json_get(classifiers, key_n32);
                    if (n32) {
                        lib->is_natives = 1;
                        strcpy(lib->natives_key, "natives-windows-32");
                    }
                }
            }
        }
    } else {
        // No downloads block - legacy format: use top-level url field as maven base
        lib->is_required = 1;
        const char *url = mc_json_get_string(lib_json, "url", "");
        if (url && *url) {
            size_t len = strlen(url);
            while (len > 0 && url[len - 1] == '/') len--;
            if (len < sizeof(lib->url) - 1) {
                memcpy(lib->url, url, len);
                lib->url[len] = '\0';
            }
        }
    }

    // Check natives field (old format)
    McJson *natives = mc_json_get(lib_json, "natives");
    if (natives && !lib->is_natives) {
        const char *nw = mc_json_get_string(natives, "windows", NULL);
        if (nw) {
            lib->is_natives = 1;
            strncpy(lib->natives_key, nw, sizeof(lib->natives_key) - 1);
        }
    }

    // Parse extract excludes
    McJson *extract = mc_json_get(lib_json, "extract");
    if (extract) {
        McJson *exclude = mc_json_get(extract, "exclude");
        if (exclude && exclude->type == MC_JSON_ARRAY) {
            int n = mc_json_array_length(exclude);
            for (int i = 0; i < n && lib->exclude_count < 8; i++) {
                McJson *item = mc_json_get_array_item(exclude, i);
                if (item && item->type == MC_JSON_STRING && item->string_value) {
                    strncpy(lib->extract_exclude[lib->exclude_count++],
                            item->string_value, 63);
                }
            }
        }
    }

    v->library_count++;
}

static int rule_matches_current_os(McJson *os) {
    if (!os) return 1;
    const char *current_os = mc_platform_get();
    const char *current_arch = mc_platform_arch_get();
    // New format (1.20+): os is a string like "@{name=windows}" or "@{arch=x86}"
    if (os->type == MC_JSON_STRING && os->string_value) {
        const char *s = os->string_value;
        if (s[0] == '@' && s[1] == '{') {
            s += 2;
            const char *end = s + strlen(s) - 1;
            if (*end == '}') {
                char cond[256];
                size_t len = (size_t)(end - s);
                if (len >= sizeof(cond)) len = sizeof(cond) - 1;
                memcpy(cond, s, len);
                cond[len] = '\0';
                char *eq = strchr(cond, '=');
                if (eq) {
                    *eq = '\0';
                    const char *key = cond;
                    const char *val = eq + 1;
                    if (strcmp(key, "name") == 0) {
                        return (strcmp(val, current_os) == 0) ? 1 : 0;
                    } else if (strcmp(key, "arch") == 0) {
                        if (strcmp(val, "x86") == 0)
                            return (strcmp(current_arch, "x86") == 0) ? 1 : 0;
                        if (strcmp(val, "x64") == 0)
                            return (strcmp(current_arch, "x64") == 0) ? 1 : 0;
                    }
                }
            }
        }
        return 1;
    }
    // Old format: os is an object {"name": "windows"}
    const char *name = mc_json_get_string(os, "name", "");
    if (name && *name) {
        if (strcmp(name, current_os) != 0) return 0;
    }
    const char *arch = mc_json_get_string(os, "arch", "");
    if (arch && *arch) {
        if (strcmp(arch, "x86") == 0)
            return (strcmp(current_arch, "x86") == 0) ? 1 : 0;
    }
    return 1;
}

// Check if features condition matches (1.20+ format)
// Features like is_demo_user=false, has_custom_resolution=false, etc. should NOT match
static int feature_matches(McJson *features) {
    if (!features || features->type != MC_JSON_OBJECT) return 1;
    // Check each feature key-value pair
    // If ANY feature doesn't match, the rule doesn't apply
    for (McJson *f = features->child; f; f = f->next) {
        if (!f->key) continue;
        // All known features that we DON'T want enabled
        // is_demo_user: we're not in demo mode
        // has_custom_resolution: we don't pass custom resolution
        // has_quick_plays_support / is_quick_play_*: we don't support quick plays
        if (strcmp(f->key, "is_demo_user") == 0) {
            // If feature is true, we're in demo mode (we're not)
            // If feature is false or missing, we're NOT in demo mode -> rule should NOT match
            if (f->type == MC_JSON_NUMBER && f->number_value != 0) return 0;
            if (f->type == MC_JSON_BOOL && f->bool_value) return 0;
        }
        if (strcmp(f->key, "has_custom_resolution") == 0) {
            if (f->type == MC_JSON_NUMBER && f->number_value != 0) return 0;
            if (f->type == MC_JSON_BOOL && f->bool_value) return 0;
        }
        if (strcmp(f->key, "has_quick_plays_support") == 0) {
            if (f->type == MC_JSON_NUMBER && f->number_value != 0) return 0;
            if (f->type == MC_JSON_BOOL && f->bool_value) return 0;
        }
        if (strstr(f->key, "is_quick_play") == f->key) {
            if (f->type == MC_JSON_NUMBER && f->number_value != 0) return 0;
            if (f->type == MC_JSON_BOOL && f->bool_value) return 0;
        }
    }
    // All features are false/missing -> rule doesn't match
    return 0;
}

int mc_version_evaluate_rules(McJson *rules_node) {
    if (!rules_node) return 1;
    // Handle single-object rule (not wrapped in array)
    if (rules_node->type == MC_JSON_OBJECT) {
        const char *action = mc_json_get_string(rules_node, "action", "");
        McJson *os = mc_json_get(rules_node, "os");
        McJson *features = mc_json_get(rules_node, "features");
        if (os) {
            if (rule_matches_current_os(os))
                return (strcmp(action, "allow") == 0) ? 1 : 0;
            return 0;
        }
        if (features) {
            if (feature_matches(features))
                return (strcmp(action, "allow") == 0) ? 1 : 0;
            return 0;
        }
        return (strcmp(action, "allow") == 0) ? 1 : 0;
    }
    if (rules_node->type != MC_JSON_ARRAY) return 0;
    int result = 0;
    int n = mc_json_array_length(rules_node);
    for (int i = 0; i < n; i++) {
        McJson *rule = mc_json_get_array_item(rules_node, i);
        if (!rule) continue;
        McJson *inner = rule;
        // Handle array items that are also single objects
        if (inner->type == MC_JSON_OBJECT) {
            const char *action = mc_json_get_string(inner, "action", "");
            McJson *os = mc_json_get(inner, "os");
            McJson *features = mc_json_get(inner, "features");
            if (os) {
                if (rule_matches_current_os(os))
                    result = (strcmp(action, "allow") == 0) ? 1 : 0;
            } else if (features) {
                if (feature_matches(features))
                    result = (strcmp(action, "allow") == 0) ? 1 : 0;
            } else {
                // Rule without OS or features restriction applies unconditionally
                result = (strcmp(action, "allow") == 0) ? 1 : 0;
            }
        }
    }
    return result;
}

int mc_version_parse(McVersion *v, const char *json_data) {
    if (!v || !json_data) return 0;
    McJson *j = mc_json_parse(json_data);
    if (!j || j->type != MC_JSON_OBJECT) { mc_json_free(j); return 0; }
    v->raw_json = j;

    const char *s;
    s = mc_json_get_string(j, "id", ""); strncpy(v->id, s, sizeof(v->id) - 1);
    s = mc_json_get_string(j, "type", ""); strncpy(v->type, s, sizeof(v->type) - 1);
    s = mc_json_get_string(j, "mainClass", ""); strncpy(v->main_class, s, sizeof(v->main_class) - 1);
    s = mc_json_get_string(j, "minecraftArguments", ""); strncpy(v->minecraft_arguments, s, sizeof(v->minecraft_arguments) - 1);
    s = mc_json_get_string(j, "inheritsFrom", ""); strncpy(v->inherits_from, s, sizeof(v->inherits_from) - 1);
    s = mc_json_get_string(j, "jar", ""); strncpy(v->jar, s, sizeof(v->jar) - 1);
    s = mc_json_get_string(j, "assets", ""); strncpy(v->assets, s, sizeof(v->assets) - 1);

    // Asset index
    McJson *ai = mc_json_get(j, "assetIndex");
    if (ai) {
        s = mc_json_get_string(ai, "id", ""); strncpy(v->asset_index.id, s, sizeof(v->asset_index.id) - 1);
        s = mc_json_get_string(ai, "url", ""); strncpy(v->asset_index.url, s, sizeof(v->asset_index.url) - 1);
        s = mc_json_get_string(ai, "sha1", ""); strncpy(v->asset_index.sha1, s, sizeof(v->asset_index.sha1) - 1);
        v->asset_index.size = (long)mc_json_get_number(ai, "size", 0);
        v->asset_index.total_size = (long)mc_json_get_number(ai, "totalSize", 0);
    }

    // Java version
    McJson *jv = mc_json_get(j, "javaVersion");
    if (jv) {
        v->java_major_version = (int)mc_json_get_number(jv, "majorVersion", 0);
        s = mc_json_get_string(jv, "component", ""); strncpy(v->java_component, s, sizeof(v->java_component) - 1);
    }

    // Downloads
    McJson *dl = mc_json_get(j, "downloads");
    if (dl) {
        McJson *client = mc_json_get(dl, "client");
        if (client) {
            s = mc_json_get_string(client, "url", ""); strncpy(v->client_url, s, sizeof(v->client_url) - 1);
            s = mc_json_get_string(client, "sha1", ""); strncpy(v->client_sha1, s, sizeof(v->client_sha1) - 1);
            v->client_size = (long)mc_json_get_number(client, "size", 0);
        }
        McJson *server = mc_json_get(dl, "server");
        if (server) {
            s = mc_json_get_string(server, "url", ""); strncpy(v->server_url, s, sizeof(v->server_url) - 1);
            s = mc_json_get_string(server, "sha1", ""); strncpy(v->server_sha1, s, sizeof(v->server_sha1) - 1);
            v->server_size = (long)mc_json_get_number(server, "size", 0);
        }
    }

    // Logging client
    McJson *logcfg = mc_json_get(j, "logging");
    if (logcfg) {
        McJson *client = mc_json_get(logcfg, "client");
        if (client) {
            McJson *file = mc_json_get(client, "file");
            if (file) {
                s = mc_json_get_string(file, "url", ""); strncpy(v->logging_client_url, s, sizeof(v->logging_client_url) - 1);
                s = mc_json_get_string(file, "sha1", ""); strncpy(v->logging_client_sha1, s, sizeof(v->logging_client_sha1) - 1);
            }
        }
    }

    // Libraries
    McJson *libs = mc_json_get(j, "libraries");
    if (libs && libs->type == MC_JSON_ARRAY) {
        int n = mc_json_array_length(libs);
        for (int i = 0; i < n; i++) {
            McJson *lib_json = mc_json_get_array_item(libs, i);
            if (lib_json) parse_library(v, lib_json);
        }
    }

    v->is_loaded = 1;
    return 1;
}

int mc_version_parse_file(McVersion *v, const char *path) {
    if (!v || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 0; }
    char *data = (char *)malloc((size_t)len + 1);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, (size_t)len, f);
    data[len] = '\0';
    fclose(f);
    int ok = mc_version_parse(v, data);
    free(data);
    return ok;
}

int mc_version_fetch(McVersion *v, const char *url) {
    if (!v || !url) return 0;
    strncpy(v->source_url, url, sizeof(v->source_url) - 1);
    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 30000);
    mc_info("Fetching version JSON: %s", url);
    McHttpResponse *resp = mc_http_get(&client, url);
    if (!resp || !resp->success || !resp->data) {
        mc_error("Failed to fetch version JSON from %s", url);
        if (resp) mc_http_response_free(resp);
        return 0;
    }
    int ok = mc_version_parse(v, resp->data);
    mc_http_response_free(resp);
    return ok;
}

int mc_version_fetch_by_id(McVersion *v, const char *version_id) {
    return mc_version_fetch_by_id_mirror(v, version_id, NULL);
}

int mc_version_fetch_by_id_mirror(McVersion *v, const char *version_id, const char *mirror_type) {
    if (!v || !version_id) return 0;
    McManifest *manifest = (McManifest *)malloc(sizeof(McManifest));
    if (!manifest) return 0;
    memset(manifest, 0, sizeof(McManifest));
    if (!mc_manifest_fetch_mirror(manifest, 0, mirror_type)) { free(manifest); return 0; }
    McVersionEntry entry;
    if (!mc_manifest_find(manifest, version_id, &entry)) {
        free(manifest);
        mc_error("Version '%s' not found in manifest", version_id);
        return 0;
    }
    free(manifest);
    // Translate version URL if mirror is set
    if (mirror_type && strcmp(mirror_type, "mojang") != 0) {
        char translated[512];
        if (mc_download_translate_mojang_url(entry.url, translated, sizeof(translated), mirror_type)) {
            strncpy(entry.url, translated, sizeof(entry.url) - 1);
        }
    }
    return mc_version_fetch(v, entry.url);
}

void mc_version_free(McVersion *v) {
    if (!v) return;
    mc_json_free(v->raw_json);
    v->raw_json = NULL;
    v->is_loaded = 0;
}

// ---- Cross-platform runtime detection ----
static char g_platform[16] = "";
static char g_arch[16] = "";

const char *mc_platform_get(void) {
    if (!g_platform[0]) {
#ifdef _WIN32
        strcpy(g_platform, "windows");
#elif defined(__APPLE__)
        strcpy(g_platform, "osx");
#else
        strcpy(g_platform, "linux");
#endif
    }
    return g_platform;
}

void mc_platform_set(const char *platform) {
    if (platform)
        strncpy(g_platform, platform, sizeof(g_platform) - 1);
}

const char *mc_platform_arch_get(void) {
    if (!g_arch[0]) {
#ifdef _WIN64
        strcpy(g_arch, "x64");
#elif defined(_WIN32)
        strcpy(g_arch, "x86");
#elif defined(__x86_64__) || defined(__amd64__)
        strcpy(g_arch, "x64");
#elif defined(__aarch64__) || defined(__arm64__)
        strcpy(g_arch, "arm64");
#elif defined(__arm__)
        strcpy(g_arch, "arm32");
#else
        strcpy(g_arch, "x64");
#endif
    }
    return g_arch;
}

void mc_platform_arch_set(const char *arch) {
    if (arch)
        strncpy(g_arch, arch, sizeof(g_arch) - 1);
}
