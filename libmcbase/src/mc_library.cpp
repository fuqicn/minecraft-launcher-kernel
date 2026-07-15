#include "mc_library.h"
#include "mc_str.h"
#include <cstring>
#include <sstream>

static void maven_to_path(const char *name, char *out, size_t out_size, int with_classifier, const char *classifier) {
    if (!name || !out || out_size == 0) return;
    char **parts = NULL;
    int n = mc_strsplit(name, ':', &parts);
    if (n < 3) {
        if (out_size > 0) out[0] = '\0';
        mc_strsplit_free(parts, n);
        return;
    }
    std::string group;
    for (const char *p = parts[0]; *p; p++) {
        if (*p == '.') group += '/';
        else group += *p;
    }
    const char *artifact = parts[1];
    const char *version = parts[2];
    std::ostringstream oss;
    oss << group << '/' << artifact << '/' << version << '/' << artifact << '-' << version;
    if (with_classifier && classifier && *classifier) {
        oss << '-' << classifier;
    }
    oss << ".jar";
    std::string result = oss.str();
    std::strncpy(out, result.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    mc_strsplit_free(parts, n);
}

int mc_library_resolve_path(const char *name, char *out, size_t out_size) {
    char **parts = NULL;
    int n = mc_strsplit(name, ':', &parts);
    int ret = 0;
    if (n >= 4) {
        // Has classifier (e.g. group:artifact:version:classifier)
        maven_to_path(name, out, out_size, 1, parts[3]);
    } else {
        maven_to_path(name, out, out_size, 0, NULL);
    }
    if (out[0] != '\0') ret = 1;
    mc_strsplit_free(parts, n);
    return ret;
}

int mc_library_resolve_url(const char *name, const char *mirror_base, char *out, size_t out_size) {
    char path[512];
    maven_to_path(name, path, sizeof(path), 0, NULL);
    if (!mirror_base || !*mirror_base)
        mirror_base = "https://libraries.minecraft.net";
    std::ostringstream oss;
    oss << mirror_base << '/' << path;
    std::string result = oss.str();
    std::strncpy(out, result.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

int mc_library_natives_path(const char *name, const char *natives_key, char *out, size_t out_size) {
    // For new-style entries where classifier is embedded in the name (4+ parts)
    if (!natives_key || !*natives_key) {
        char **parts = NULL;
        int n = mc_strsplit(name, ':', &parts);
        if (n >= 4) {
            maven_to_path(name, out, out_size, 1, parts[3]);
            mc_strsplit_free(parts, n);
            return out[0] != '\0';
        }
        mc_strsplit_free(parts, n);
    }
    std::string key = natives_key ? natives_key : "natives-windows";
    std::string::size_type pos = key.find("${arch}");
    if (pos != std::string::npos) {
        key.replace(pos, 7, "64");
    }
    maven_to_path(name, out, out_size, 1, key.c_str());
    return out[0] != '\0';
}
