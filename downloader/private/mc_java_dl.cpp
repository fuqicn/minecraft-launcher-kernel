#include "mc_java_dl.h"
#include <mc_http.h>
#include <mc_json.h>
#include <mc_log.h>
#include <mc_str.h>
#include <mc_download.h>
#include <mc_version.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

#include <QtCore/QDir>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>

static const char *java_manifest_url(const char *mirror) {
    if (mirror && strcmp(mirror, "bmclapi") == 0)
        return "https://bmclapi2.bangbang93.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json";
    return "https://piston-meta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json";
}

int mc_java_download_manifest(int major_version, const char *mirror, McJavaFileList *list) {
    memset(list, 0, sizeof(*list));
    const char *url = java_manifest_url(mirror);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 30000);
    McHttpResponse *resp = mc_http_get(&client, url);
    if (!resp || !resp->success || !resp->data) {
        mc_error("Failed to fetch Java runtime manifest");
        if (resp) mc_http_response_free(resp);
        return 0;
    }

    McJson *root = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!root) { mc_error("Failed to parse Java manifest JSON"); return 0; }

    const char *version_key = "jre-legacy";
    if (major_version >= 21) version_key = "java-runtime-delta";
    else if (major_version >= 17) version_key = "java-runtime-beta";
    else if (major_version >= 16) version_key = "java-runtime-alpha";

    const char *plat = mc_platform_get();
    const char *arch = mc_platform_arch_get();
    char plat_key[32];
    snprintf(plat_key, sizeof(plat_key), "%s-%s", plat, arch);
    McJson *plat_obj = mc_json_get(root, plat_key);
    if (!plat_obj) {
        snprintf(plat_key, sizeof(plat_key), "%s-x64", plat);
        plat_obj = mc_json_get(root, plat_key);
    }
    if (!plat_obj) {
        mc_error("No '%s' entry in Java manifest (tried: %s-x64, %s-%s)", plat, plat, plat, arch);
        mc_json_free(root); return 0;
    }

    McJson *rt_array = mc_json_get(plat_obj, version_key);
    if (!rt_array || rt_array->type != MC_JSON_ARRAY) {
        if (plat_obj && plat_obj->type == MC_JSON_OBJECT) {
            const char *key = nullptr;
            McJson *val = nullptr;
            if (mc_json_object_foreach(plat_obj, 0, &key, &val)) {
                rt_array = val;
                version_key = key;
            }
        }
    }
    if (!rt_array || rt_array->type != MC_JSON_ARRAY || !rt_array->child) {
        mc_error("No Java runtime '%s' found in manifest", version_key);
        mc_json_free(root);
        return 0;
    }

    McJson *rt_entry = mc_json_get_array_item(rt_array, 0);
    if (!rt_entry) {
        mc_error("Empty Java runtime array");
        mc_json_free(root);
        return 0;
    }

    McJson *manifest = mc_json_get(rt_entry, "manifest");
    if (!manifest) {
        mc_error("No manifest in Java runtime entry");
        mc_json_free(root);
        return 0;
    }
    const char *manifest_url = mc_json_get_string(manifest, "url", nullptr);
    if (!manifest_url) {
        mc_error("No manifest URL in Java runtime entry");
        mc_json_free(root);
        return 0;
    }

    char manifest_url_translated[2048];
    strncpy(manifest_url_translated, manifest_url, sizeof(manifest_url_translated) - 1);
    if (mirror && strcmp(mirror, "mojang") != 0) {
        char translated[2048];
        if (mc_download_translate_mojang_url(manifest_url, translated, sizeof(translated), mirror))
            strncpy(manifest_url_translated, translated, sizeof(manifest_url_translated) - 1);
    }
    mc_http_init(&client);
    mc_http_set_timeout(&client, 30000);
    resp = mc_http_get(&client, manifest_url_translated);
    if (!resp || !resp->success || !resp->data) {
        mc_error("Failed to fetch Java runtime file manifest");
        if (resp) mc_http_response_free(resp);
        mc_json_free(root);
        return 0;
    }

    McJson *file_root = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!file_root) { mc_error("Failed to parse file manifest JSON"); mc_json_free(root); return 0; }

    McJson *files = mc_json_get(file_root, "files");
    if (!files || files->type != MC_JSON_OBJECT) {
        mc_error("No files in Java runtime manifest");
        mc_json_free(file_root);
        mc_json_free(root);
        return 0;
    }

    std::vector<McJavaFile> file_list;
    for (McJson *entry = files->child; entry; entry = entry->next) {
        if (!entry->key) continue;
        const char *rel_path = entry->key;

        McJson *downloads = mc_json_get(entry, "downloads");
        if (!downloads) continue;
        McJson *raw = mc_json_get(downloads, "raw");
        if (!raw) continue;

        const char *dl_url = mc_json_get_string(raw, "url", nullptr);
        const char *dl_sha1 = mc_json_get_string(raw, "sha1", nullptr);
        long dl_size = (long)mc_json_get_number(raw, "size", 0);

        if (!dl_url || !dl_sha1) continue;

        McJavaFile f;
        memset(&f, 0, sizeof(f));
        strncpy(f.url, dl_url, sizeof(f.url) - 1);
        strncpy(f.path, rel_path, sizeof(f.path) - 1);
        strncpy(f.sha1, dl_sha1, sizeof(f.sha1) - 1);
        f.size = dl_size;
        file_list.push_back(f);
    }

    mc_json_free(file_root);
    mc_json_free(root);

    if (file_list.empty()) { mc_error("No downloadable files in Java manifest"); return 0; }

    list->count = (int)file_list.size();
    list->capacity = list->count;
    list->files = (McJavaFile *)malloc(sizeof(McJavaFile) * list->count);
    if (!list->files) { list->count = 0; return 0; }
    for (int i = 0; i < list->count; i++)
        list->files[i] = file_list[i];

    mc_info("Java manifest: %d files for %s", list->count, version_key);
    return 1;
}

void mc_java_file_list_free(McJavaFileList *list) {
    if (list && list->files) {
        free(list->files);
        list->files = nullptr;
    }
    list->count = 0;
    list->capacity = 0;
}
