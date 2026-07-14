#include <mcbase.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include "mc_auth.h"

#define ARGBUF_SIZE 1048576
#define MC_AUTH_TOKEN_SIZE 4096

static char g_player_name[64] = "Player";
static char g_mc_dir[MC_PATH_MAX] = ".";
static int  g_memory_mb = 0;
static char g_java_path[1024] = "";
static char g_extra_jvm[2048] = "";

static void print_help(void) {
    std::ostringstream oss;
    oss << "mclaunch - " << mc_i18n("mclaunch_desc") << std::endl;
    oss << mc_i18n("usage") << ": mclaunch <" << mc_i18n("version") << "> [" << mc_i18n("options") << "]" << std::endl;
    oss << std::endl;
    oss << mc_i18n("launch_options") << ":" << std::endl;
    oss << "  --dir <path>        " << mc_i18n("dir_opt") << std::endl;
    oss << "  --username <name>   " << mc_i18n("username_opt") << std::endl;
    oss << "  --memory <MB>       " << mc_i18n("memory_opt") << std::endl;
    oss << "  --java <path>       " << mc_i18n("java_opt") << std::endl;
    oss << "  --jvm <args>        " << mc_i18n("jvm_opt") << std::endl;
    oss << "  --session <file>    " << mc_i18n("session_opt") << std::endl;
    oss << "  --platform <os>     " << mc_i18n("platform_opt") << std::endl;
    oss << "  --lang <code>       " << mc_i18n("lang_opt") << std::endl;
    oss << "  --help              " << mc_i18n("help_opt") << std::endl;
    oss << std::endl;
    oss << mc_i18n("examples") << ":" << std::endl;
    oss << "  mclaunch 1.20.4 --java /path/to/java" << std::endl;
    oss << "  mclaunch 1.20.1 --dir mymc --memory 2048 --java /path/to/java" << std::endl;
    oss << "  mclaunch 1.12.2-forge-14.23.5.2864 --session session.json --java /path/to/java" << std::endl;
    oss << std::endl;
    mc_console_write(oss.str().c_str());
}

static int find_opt(const char *opt, int argc, char **argv, const char **val) {
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], opt) == 0) {
            if (val && i + 1 < argc) { *val = argv[i + 1]; return i; }
            return i;
        }
    }
    return -1;
}

static int find_opt_any(const char *opt, int argc, char **argv, const char **val) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], opt) == 0) {
            if (val && i + 1 < argc) { *val = argv[i + 1]; return i; }
            return i;
        }
    }
    return -1;
}

// Generate offline-mode UUID from username (version-3 UUID from "OfflinePlayer:<name>")
static void generate_offline_uuid(const char *name, char *out, size_t out_size) {
    unsigned char hash[16];
    char buf[256];
    snprintf(buf, sizeof(buf), "OfflinePlayer:%s", name);
    mc_hash_data_md5(reinterpret_cast<const unsigned char *>(buf), strlen(buf), hash);
    // Set version to 3 and variant to RFC 4122
    hash[6] = (hash[6] & 0x0f) | 0x30;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    snprintf(out, out_size,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        hash[0], hash[1], hash[2], hash[3],
        hash[4], hash[5], hash[6], hash[7],
        hash[8], hash[9], hash[10], hash[11],
        hash[12], hash[13], hash[14], hash[15]);
}

static int build_classpath(char *buf, size_t buf_size, McVersion *v, const char *mc_dir) {
    char abs_mc_dir[MC_PATH_MAX];
    mc_path_normalize(mc_dir, abs_mc_dir, sizeof(abs_mc_dir));

    size_t pos = 0;
    buf[0] = '\0';

    // Add the version's client JAR
    char jar_file[MC_PATH_MAX];
    const char *jar_name = v->jar[0] ? v->jar : v->id;
    snprintf(jar_file, sizeof(jar_file), "%s%cversions%c%s%c%s.jar",
        abs_mc_dir, QDir::separator().toLatin1(), QDir::separator().toLatin1(),
        jar_name, QDir::separator().toLatin1(), jar_name);
    if (mc_path_exists(jar_file)) {
        size_t len = strlen(jar_file);
        if (pos + len + 2 <= buf_size) {
            memcpy(buf + pos, jar_file, len);
            pos += len;
            buf[pos] = '\0';
        }
    }

    // Add inherited version's JAR if different
    if (v->inherits_from[0] && strcmp(v->inherits_from, jar_name) != 0) {
        char base_jar[MC_PATH_MAX];
        snprintf(base_jar, sizeof(base_jar), "%s%cversions%c%s%c%s.jar",
            abs_mc_dir, QDir::separator().toLatin1(), QDir::separator().toLatin1(),
            v->inherits_from, QDir::separator().toLatin1(), v->inherits_from);
        if (mc_path_exists(base_jar)) {
            size_t len = strlen(base_jar);
            if (pos + len + 2 <= buf_size) {
                if (pos > 0) buf[pos++] = QDir::listSeparator().toLatin1();
                memcpy(buf + pos, base_jar, len);
                pos += len;
                buf[pos] = '\0';
            }
        }
    }

    // Add libraries
    char libraries_dir[MC_PATH_MAX];
    mc_path_join(abs_mc_dir, "libraries", libraries_dir, sizeof(libraries_dir));

    for (int i = 0; i < v->library_count; i++) {
        McLibrary *lib = &v->libraries[i];
        if (!lib->is_required) continue;
        if (lib->is_natives && lib->classifier_url[0]) continue;
        char rel_path[MC_PATH_MAX];
        mc_library_resolve_path(lib->name, rel_path, sizeof(rel_path));
        if (!rel_path[0]) continue;

        char full[MC_PATH_MAX];
        mc_path_join(libraries_dir, rel_path, full, sizeof(full));

            // For known native entries, keep the native JAR on classpath (LWJGL 3 needs it)
            if (!lib->is_natives) {
                // Handle native classifiers: look for non-native JAR (strip natives classifier)
            char alt_path[MC_PATH_MAX];
            mc_path_dirname(full, alt_path, sizeof(alt_path));
            const char *fname = mc_path_filename(full);
            if (!fname) continue;
            const char *native_marker = strstr(fname, "-natives-");
            if (native_marker) {
                std::string base(fname, native_marker - fname);
                std::string ext = ".jar";
                const char *dot = strrchr(fname, '.');
                if (dot) ext = dot;
                std::string alt_name = base + ext;
                mc_path_join(alt_path, alt_name.c_str(), full, sizeof(full));
                if (!mc_path_exists(full)) continue;
            }
            }

        if (!mc_path_exists(full)) continue;

        size_t len = strlen(full);
        if (pos + len + 2 <= buf_size) {
            if (pos > 0) buf[pos++] = QDir::listSeparator().toLatin1();
            memcpy(buf + pos, full, len);
            pos += len;
            buf[pos] = '\0';
        }
    }

    // Also add inherited version's libraries
    if (v->inherits_from[0]) {
        McVersion *base = (McVersion *)malloc(sizeof(McVersion));
        if (!base) return 0;
        mc_version_init(base);
        char base_json_file[MC_PATH_MAX];
        snprintf(base_json_file, sizeof(base_json_file), "%s%cversions%c%s%c%s.json",
            abs_mc_dir, QDir::separator().toLatin1(), QDir::separator().toLatin1(),
            v->inherits_from, QDir::separator().toLatin1(), v->inherits_from);
        if (mc_path_exists(base_json_file) && mc_version_parse_file(base, base_json_file)) {
            for (int i = 0; i < base->library_count; i++) {
                McLibrary *lib = &base->libraries[i];
                if (!lib->is_required) continue;
        if (lib->is_natives && lib->classifier_url[0]) continue;
                char rel_path[MC_PATH_MAX];
                mc_library_resolve_path(lib->name, rel_path, sizeof(rel_path));
                if (!rel_path[0]) continue;
                char full[MC_PATH_MAX];
                mc_path_join(libraries_dir, rel_path, full, sizeof(full));
                if (!mc_path_exists(full)) continue;
                // Check not already in classpath
                if (strstr(buf, full)) continue;
                size_t len = strlen(full);
                if (pos + len + 2 <= buf_size) {
                    if (pos > 0) buf[pos++] = QDir::listSeparator().toLatin1();
                    memcpy(buf + pos, full, len);
                    pos += len;
                    buf[pos] = '\0';
                }
            }
        }
        mc_version_free(base);
        free(base);
    }

    return 1;
}

static void build_jvm_args(QStringList &args, McVersion *v,
    const char *classpath, const char *natives_dir, const char *mc_dir,
    const char *libraries_dir)
{
    // Memory
    if (g_memory_mb > 0) {
        args << QString("-Xmx%1m").arg(g_memory_mb);
        args << QString("-Xms%1m").arg(g_memory_mb / 2);
    }

    // Extra JVM args
    if (g_extra_jvm[0]) {
        QString extra = QString::fromUtf8(g_extra_jvm);
        QStringList parts = extra.split(' ', Qt::SkipEmptyParts);
        for (const QString &p : parts)
            args << p.trimmed();
    }

    // Classpath
    // Always use -cp (classpath). Module path (-p) causes Java 9+ module system
    // to try deriving module names from jar filenames, which breaks jars named
    // like "1.20.1.jar". The version JSON's arguments.jvm already supplies -cp.
    args << "-DlegacyClassPath.file=" + QString::fromUtf8(classpath);
    args << "-cp" << QString::fromUtf8(classpath);

    // Log4j patch
    args << "-Dlog4j2.formatMsgNoLookups=true";

    // Extract JVM arguments from raw_json if available (modern format)
    if (!v->raw_json.isEmpty()) {
        QJsonObject args_node = v->raw_json.value("arguments").toObject();
        if (!args_node.isEmpty()) {
            QJsonArray jvm = args_node.value("jvm").toArray();
            if (!jvm.isEmpty()) {
                for (int i = 0; i < jvm.size(); i++) {
                    if (jvm[i].isString()) {
                        std::string s(jvm[i].toString().toUtf8().constData());
                        size_t p;
                        while ((p = s.find("${natives_directory}")) != std::string::npos)
                            s.replace(p, 21, natives_dir);
                        while ((p = s.find("${library_directory}")) != std::string::npos)
                            s.replace(p, 20, libraries_dir);
                        while ((p = s.find("${classpath}")) != std::string::npos)
                            s.replace(p, 12, classpath);
                        while ((p = s.find("${classpath_separator}")) != std::string::npos)
                            s.replace(p, 22, ";");
                        args << QString::fromUtf8(s.c_str());
                    }
                }
            }
        }
    }

    // Set natives directory
    args << QString("-Djava.library.path=%1").arg(QString::fromUtf8(natives_dir));
}

static void build_game_args(QStringList &args, McVersion *v,
    const char *username, const char *uuid_str,
    const char *access_token, const char *user_type,
    const char *version_type, const char *mc_dir)
{
    // Build paths for asset root and game directory
    char assets_root[1024];
    mc_path_join(mc_dir, "assets", assets_root, sizeof(assets_root));

    // Try extracting game arguments from raw_json (modern format)
    int used_modern = 0;
    if (!v->raw_json.isEmpty()) {
        QJsonObject args_node = v->raw_json.value("arguments").toObject();
        if (!args_node.isEmpty()) {
            QJsonArray game = args_node.value("game").toArray();
            if (!game.isEmpty()) {
                used_modern = 1;
                for (int i = 0; i < game.size(); i++) {
                    if (game[i].isString()) {
                        std::string s(game[i].toString().toUtf8().constData());
                        size_t p;
                        while ((p = s.find("${auth_player_name}")) != std::string::npos)
                            s.replace(p, 19, username);
                        while ((p = s.find("${auth_uuid}")) != std::string::npos)
                            s.replace(p, 12, uuid_str);
                        while ((p = s.find("${auth_access_token}")) != std::string::npos)
                            s.replace(p, 20, access_token);
                        while ((p = s.find("${user_type}")) != std::string::npos)
                            s.replace(p, 12, user_type);
                        while ((p = s.find("${version_name}")) != std::string::npos)
                            s.replace(p, 15, v->id);
                        while ((p = s.find("${assets_root}")) != std::string::npos)
                            s.replace(p, 14, assets_root);
                        while ((p = s.find("${game_directory}")) != std::string::npos)
                            s.replace(p, 17, mc_dir);
                        while ((p = s.find("${assets_index_name}")) != std::string::npos)
                            s.replace(p, 20, v->asset_index.id);
                        mc_debug("game arg: [%s] -> [%s]", game[i].toString().toUtf8().constData(), s.c_str());
                        args << QString::fromUtf8(s.c_str());
                    }
                }
            }
        }
    }

    if (!used_modern && v->minecraft_arguments[0]) {
        // Legacy minecraft_arguments string
        std::string str(v->minecraft_arguments);
        size_t p;

        while ((p = str.find("${auth_player_name}")) != std::string::npos)
            str.replace(p, 19, username);
        while ((p = str.find("${auth_uuid}")) != std::string::npos)
            str.replace(p, 12, uuid_str);
        while ((p = str.find("${auth_access_token}")) != std::string::npos)
            str.replace(p, 20, access_token);
        while ((p = str.find("${user_type}")) != std::string::npos)
            str.replace(p, 12, user_type);
        while ((p = str.find("${version_name}")) != std::string::npos)
            str.replace(p, 15, v->id);
        while ((p = str.find("${assets_root}")) != std::string::npos)
            str.replace(p, 14, assets_root);
        while ((p = str.find("${game_directory}")) != std::string::npos)
            str.replace(p, 17, mc_dir);
        while ((p = str.find("${version_type}")) != std::string::npos)
            str.replace(p, 14, version_type);
        while ((p = str.find("${assets_index_name}")) != std::string::npos)
            str.replace(p, 20, v->asset_index.id);
        while ((p = str.find("${user_properties}")) != std::string::npos)
            str.replace(p, 18, "{}");
        while ((p = str.find("${natives_directory}")) != std::string::npos)
            str.replace(p, 21, mc_dir);

        // Split by spaces respecting quotes
        std::string current;
        bool in_quote = false;
        for (char c : str) {
            if (c == '"') { in_quote = !in_quote; continue; }
            if (c == ' ' && !in_quote) {
                if (!current.empty()) { args << QString::fromUtf8(current.c_str()); current.clear(); }
                continue;
            }
            current += c;
        }
        if (!current.empty()) args << QString::fromUtf8(current.c_str());
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    mc_console_init();
    mc_log_set_level(MC_LOG_DEBUG);

    const char *lang = nullptr;
    if (find_opt_any("--lang", argc, argv, &lang) >= 0 && lang) mc_i18n_set(lang);

    const char *opt_dir = nullptr, *opt_user = nullptr;
    const char *opt_mem = nullptr, *opt_java = nullptr, *opt_jvm = nullptr;
    const char *opt_session = nullptr, *opt_platform = nullptr;

    if (argc < 2) { print_help(); return 0; }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) { print_help(); return 0; }

    const char *version_id = argv[1];

    find_opt("--dir",      argc, argv, &opt_dir);
    find_opt("--username", argc, argv, &opt_user);
    find_opt("--memory",   argc, argv, &opt_mem);
    find_opt("--java",     argc, argv, &opt_java);
    find_opt("--jvm",      argc, argv, &opt_jvm);
    find_opt("--session",  argc, argv, &opt_session);
    find_opt("--platform", argc, argv, &opt_platform);

    if (opt_dir)      strncpy(g_mc_dir, opt_dir, sizeof(g_mc_dir) - 1);
    if (opt_user)     strncpy(g_player_name, opt_user, sizeof(g_player_name) - 1);
    if (opt_mem)      g_memory_mb = atoi(opt_mem);
    if (opt_java)     strncpy(g_java_path, opt_java, sizeof(g_java_path) - 1);
    if (opt_jvm)      strncpy(g_extra_jvm, opt_jvm, sizeof(g_extra_jvm) - 1);
    if (opt_platform) mc_platform_set(opt_platform);

    // --java is required
    if (!g_java_path[0]) {
        mc_error("--java <path> is required. Use mcjava list to find available Java runtimes.");
        return 1;
    }
    if (!mc_path_exists(g_java_path)) {
        mc_error("Java not found at: %s", g_java_path);
        return 1;
    }
    mc_info("%s: %s", mc_i18n("using_java"), g_java_path);

    // Normalize mc dir
    char abs_mc_dir[MC_PATH_MAX];
    mc_path_normalize(g_mc_dir, abs_mc_dir, sizeof(abs_mc_dir));
    strncpy(g_mc_dir, abs_mc_dir, sizeof(g_mc_dir) - 1);

    // Load version JSON
    char ver_json_path[MC_PATH_MAX];
    mc_path_join3(g_mc_dir, "versions", version_id, ver_json_path, sizeof(ver_json_path));
    char ver_json_file[MC_PATH_MAX];
    snprintf(ver_json_file, sizeof(ver_json_file), "%s%c%s.json",
        ver_json_path, QDir::separator().toLatin1(), version_id);
    if (!mc_path_exists(ver_json_file)) {
        mc_error("%s: %s.", mc_i18n("error"), ver_json_file);
        return 1;
    }

    McVersion *v = static_cast<McVersion *>(malloc(sizeof(McVersion)));
    if (!v) return 1;
    mc_version_init(v);
    if (!mc_version_parse_file(v, ver_json_file)) {
        mc_error("%s: %s", mc_i18n("failed_parse_version"), ver_json_file);
        free(v);
        return 1;
    }
    mc_info("%s: %s (%s)", mc_i18n("loaded_version"), v->id, v->type);
    mc_info("%s: %s", mc_i18n("main_class"), v->main_class);

    // Find client JAR
    char jar_file[MC_PATH_MAX];
    const char *jar_name = v->jar[0] ? v->jar : v->id;
    snprintf(jar_file, sizeof(jar_file), "%s%cversions%c%s%c%s.jar",
        g_mc_dir, QDir::separator().toLatin1(), QDir::separator().toLatin1(),
        jar_name, QDir::separator().toLatin1(), jar_name);
    if (!mc_path_exists(jar_file) && v->inherits_from[0]) {
        snprintf(jar_file, sizeof(jar_file), "%s%cversions%c%s%c%s.jar",
            g_mc_dir, QDir::separator().toLatin1(), QDir::separator().toLatin1(),
            v->inherits_from, QDir::separator().toLatin1(), v->inherits_from);
    }
    if (!mc_path_exists(jar_file)) {
        mc_error("%s: %s.", mc_i18n("client_jar_not_found"), jar_file);
        mc_version_free(v); free(v);
        return 1;
    }

    // Build classpath
    char *classpath = static_cast<char *>(malloc(ARGBUF_SIZE));
    if (!classpath) { mc_version_free(v); free(v); return 1; }
    classpath[0] = '\0';
    build_classpath(classpath, ARGBUF_SIZE, v, g_mc_dir);
    mc_info("%s: %d %s", mc_i18n("classpath_chars"), static_cast<int>(strlen(classpath)), mc_i18n("bytes"));

    // Auth: load session or offline
    char uuid_str[40] = "";
    char access_token[MC_AUTH_TOKEN_SIZE] = "";
    char user_type[16] = "legacy";
    int is_online = 0;

    char session_path[MC_PATH_MAX] = "";
    if (opt_session) {
        strncpy(session_path, opt_session, sizeof(session_path) - 1);
    } else {
        char appdata[MC_PATH_MAX];
        if (mc_path_appdata(appdata, sizeof(appdata)))
            snprintf(session_path, sizeof(session_path), "%s/session.json", appdata);
    }

    if (session_path[0] && mc_path_exists(session_path)) {
        McAuthSession auth;
        mc_auth_init(&auth);
        if (mc_auth_load(&auth, session_path)) {
            mc_info("Loaded session for %s, refreshing...", auth.name);
            if (mc_auth_refresh(&auth)) {
                mc_info("Session refreshed");
                mc_auth_save(&auth, session_path);
                strncpy(uuid_str, auth.uuid, sizeof(uuid_str) - 1);
                strncpy(access_token, auth.access_token, sizeof(access_token) - 1);
                strncpy(user_type, auth.user_type, sizeof(user_type) - 1);
                if (!g_player_name[0] || strcmp(g_player_name, "Player") == 0)
                    strncpy(g_player_name, auth.name, sizeof(g_player_name) - 1);
                mc_debug("uuid=[%s] access_token(length=%zu) user_type=[%s]", uuid_str, strlen(access_token), user_type);
                is_online = 1;
            } else {
                mc_warn("Session refresh failed: %s", auth.error[0] ? auth.error : "unknown error");
            }
        } else {
            mc_warn("Failed to load session: %s", session_path);
        }
    }

    if (!is_online) {
        mc_info("Using offline mode for %s", g_player_name);
        generate_offline_uuid(g_player_name, uuid_str, sizeof(uuid_str));
        snprintf(access_token, sizeof(access_token), "offline-%s", uuid_str);
    }

    // Natives directory with inheritance fallback
    char natives_dir[MC_PATH_MAX];
    snprintf(natives_dir, sizeof(natives_dir), "%s/natives-%s", g_mc_dir, v->id);
    if (!mc_path_exists(natives_dir) && v->inherits_from[0]) {
        char base_natives[MC_PATH_MAX];
        snprintf(base_natives, sizeof(base_natives), "%s/natives-%s", g_mc_dir, v->inherits_from);
        if (mc_path_exists(base_natives))
            strncpy(natives_dir, base_natives, sizeof(natives_dir) - 1);
    }
    char libraries_dir[MC_PATH_MAX];
    mc_path_join(g_mc_dir, "libraries", libraries_dir, sizeof(libraries_dir));

    // Build command line
    QStringList args;
    build_jvm_args(args, v, classpath, natives_dir, g_mc_dir, libraries_dir);

    args.append(QString::fromUtf8(v->main_class));

    build_game_args(args, v,
        g_player_name, uuid_str, access_token, user_type, v->type, g_mc_dir);

    mc_info("Launching %s...", v->id);

    // Build command line string for display
    QString cmdline_str = QString("\"%1\"").arg(QString::fromUtf8(g_java_path));
    for (const QString &a : args) {
        if (a.contains(' ') || a.contains('"'))
            cmdline_str += " \"" + a + "\"";
        else
            cmdline_str += " " + a;
    }
    mc_debug("Full cmdline:\n%s", cmdline_str.toUtf8().constData());

    // Write cmdline to file
    FILE *cmdf = fopen("mclaunch_cmdline.txt", "w");
    if (cmdf) { fputs(cmdline_str.toUtf8().constData(), cmdf); fclose(cmdf); }

    // Launch with QProcess::startDetached
    qint64 pid = 0;
    if (!QProcess::startDetached(QString::fromUtf8(g_java_path), args,
                                  QString::fromUtf8(g_mc_dir), &pid))
    {
        mc_error("Failed to start process");
        free(classpath);
        mc_version_free(v);
        free(v);
        return 1;
    }

    mc_info("Process started (PID: %lld)", static_cast<long long>(pid));

    free(classpath);
    mc_version_free(v);
    free(v);
    return 0;
}
