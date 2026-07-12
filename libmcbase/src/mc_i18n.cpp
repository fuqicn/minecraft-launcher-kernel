#include "mc_i18n.h"
#include "mc_json.h"
#include "mc_path.h"
#include <cstring>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char g_lang[16] = "";
static McJson *g_table = NULL;
static int g_loaded = 0;

static const char *detect_system_lang(void) {
    static char buf[16] = {0};
    if (buf[0]) return buf;
    WCHAR wbuf[16] = {0};
    GetLocaleInfoW(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO639LANGNAME, wbuf, 8);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (!buf[0]) strcpy(buf, "en");
    return buf;
}

static int has_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *lang_file_path(const char *lang) {
    static char path[1024];
    char exe_dir[1024];
    if (mc_path_exe_dir(exe_dir, sizeof(exe_dir)) && exe_dir[0]) {
        snprintf(path, sizeof(path), "%s/lang/%s.json", exe_dir, lang);
        if (has_file(path)) return path;
    }
    snprintf(path, sizeof(path), "lang/%s.json", lang);
    return path;
}

void mc_i18n_set(const char *lang) {
    if (g_table) { mc_json_free(g_table); g_table = NULL; }
    g_loaded = 0;
    if (lang) {
        strncpy(g_lang, lang, sizeof(g_lang) - 1);
        g_lang[sizeof(g_lang) - 1] = '\0';
    }
}

const char *mc_i18n(const char *key) {
    if (!key) return "";

    if (!g_loaded) {
        const char *lang = g_lang[0] ? g_lang : detect_system_lang();
        const char *path = lang_file_path(lang);
        g_table = mc_json_parse_file(path);
        if (!g_table) {
            path = lang_file_path("en");
            g_table = mc_json_parse_file(path);
        }
        g_loaded = 1;
    }

    if (g_table) {
        const char *val = mc_json_get_string(g_table, key, NULL);
        if (val) return val;
    }
    return key;
}
