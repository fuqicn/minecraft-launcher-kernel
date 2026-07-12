#include <mcbase.h>
#include <iostream>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <cstdio>

static void print_help(void) {
    std::cout << "mcver - " << mc_i18n("version") << std::endl;
    std::cout << mc_i18n("usage") << ": mcver <" << mc_i18n("commands") << "> [" << mc_i18n("options") << "]\n" << std::endl;
    std::cout << mc_i18n("commands") << ":" << std::endl;
    std::cout << "  list [--type <release|snapshot|old|all>]   " << mc_i18n("info") << std::endl;
    std::cout << "  search <query>                              " << mc_i18n("search_results") << std::endl;
    std::cout << "  info <version>                              " << mc_i18n("info") << std::endl;
    std::cout << "  manifest [--refresh]                        " << mc_i18n("refresh") << std::endl;
    std::cout << "  latest                                      " << mc_i18n("latest_release") << " " << mc_i18n("info") << std::endl;
    std::cout << "  help                                        " << mc_i18n("help") << "\n" << std::endl;
    std::cout << mc_i18n("options") << ":" << std::endl;
    std::cout << "  --mirror <type>   " << mc_i18n("mirror") << ": mojang, bmclapi, mcbbs\n" << std::endl;
    std::cout << "  --lang <code>     " << mc_i18n("lang_opt") << std::endl;
    std::cout << mc_i18n("examples") << ":" << std::endl;
    std::cout << "  mcver list" << std::endl;
    std::cout << "  mcver list --type release" << std::endl;
    std::cout << "  mcver search 1.20" << std::endl;
    std::cout << "  mcver info 1.20.4" << std::endl;
    std::cout << "  mcver manifest --refresh" << std::endl;
    std::cout << "  mcver latest" << std::endl;
}

static const char *g_mirror = nullptr;

static McManifest *fetch_manifest(int force_refresh) {
    McManifest *m = (McManifest *)malloc(sizeof(McManifest));
    if (!m) return nullptr;
    memset(m, 0, sizeof(McManifest));
    if (!mc_manifest_fetch_mirror(m, force_refresh, g_mirror)) {
        free(m);
        return nullptr;
    }
    return m;
}

static void cmd_list(int argc, char **argv) {
    const char *filter = "all";
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--type") == 0 && i + 1 < argc)
            filter = argv[++i];
    }
    McManifest *m = fetch_manifest(0);
    if (!m) {
        std::cout << mc_i18n("error") << ": " << mc_i18n("no_matching") << std::endl;
        return;
    }
    std::cout << std::left << std::setw(24) << "ID" << std::setw(12) << mc_i18n("type") << mc_i18n("release_time") << std::endl;
    std::cout << std::left << std::setw(24) << "----" << std::setw(12) << "----" << "------------" << std::endl;
    for (int i = 0; i < m->count; i++) {
        if (strcmp(filter, "all") == 0 || strcmp(m->entries[i].type, filter) == 0) {
            time_t t = (time_t)m->entries[i].release_time;
            struct tm *tm = localtime(&t);
            char timebuf[32];
            if (tm) strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", tm);
            else strcpy(timebuf, "unknown");
            std::cout << std::left << std::setw(24) << m->entries[i].id << std::setw(12) << m->entries[i].type << timebuf << std::endl;
        }
    }
    std::cout << "\n" << mc_i18n("total_versions") << ": " << m->count << std::endl;
    std::cout << mc_i18n("latest_release") << ": " << m->latest_release << std::endl;
    std::cout << mc_i18n("latest_snapshot") << ": " << m->latest_snapshot << std::endl;
    free(m);
}

static void cmd_search(int argc, char **argv) {
    if (argc < 3) { std::cout << mc_i18n("usage") << ": mcver search <query>" << std::endl; return; }
    const char *query = argv[2];
    McManifest *m = fetch_manifest(0);
    if (!m) { std::cout << mc_i18n("error") << ": " << mc_i18n("no_matching") << std::endl; return; }
    McVersionEntry results[100];
    int found = mc_manifest_search(m, query, results, 100);
    if (found == 0) { std::cout << mc_i18n("no_matching") << " '" << query << "'" << std::endl; free(m); return; }
    std::cout << mc_i18n("found_versions") << " '" << query << "': " << found << "\n" << std::endl;
    std::cout << std::left << std::setw(24) << "ID" << std::setw(12) << mc_i18n("type") << mc_i18n("release_time") << std::endl;
    std::cout << std::left << std::setw(24) << "----" << std::setw(12) << "----" << "------------" << std::endl;
    for (int i = 0; i < found; i++) {
        time_t t = (time_t)results[i].release_time;
        struct tm *tm = localtime(&t);
        char timebuf[32];
        if (tm) strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", tm);
        else strcpy(timebuf, "unknown");
        std::cout << std::left << std::setw(24) << results[i].id << std::setw(12) << results[i].type << timebuf << std::endl;
    }
    free(m);
}

static void cmd_info(int argc, char **argv) {
    if (argc < 3) { std::cout << mc_i18n("usage") << ": mcver info <version>" << std::endl; return; }
    McVersion *v = (McVersion *)malloc(sizeof(McVersion));
    if (!v) { std::cout << mc_i18n("error") << ": " << mc_i18n("failed_parse_version") << std::endl; return; }
    mc_version_init(v);
    int ok;
    if (g_mirror) ok = mc_version_fetch_by_id_mirror(v, argv[2], g_mirror);
    else ok = mc_version_fetch_by_id(v, argv[2]);
    if (!ok) {
        std::cout << mc_i18n("error") << ": " << mc_i18n("failed_parse_version") << " '" << argv[2] << "'" << std::endl;
        free(v);
        return;
    }
    std::cout << mc_i18n("version") << ": " << v->id << std::endl;
    std::cout << "  " << mc_i18n("type") << ": " << v->type << std::endl;
    std::cout << "  " << mc_i18n("main_class") << ": " << v->main_class << std::endl;
    std::cout << "  " << mc_i18n("java_version") << ": " << v->java_major_version << std::endl;
    std::cout << "  " << mc_i18n("inherits_from") << ": " << v->inherits_from << std::endl;
    std::cout << "  " << mc_i18n("libraries") << ": " << v->library_count << std::endl;
    std::cout << "  " << mc_i18n("client_size") << ": " << v->client_size << " " << mc_i18n("bytes") << std::endl;
    std::cout << "  " << mc_i18n("asset_index") << ": " << v->asset_index.id << std::endl;
    if (v->assets[0]) std::cout << "  " << mc_i18n("assets") << ": " << v->assets << std::endl;
    if (v->minecraft_arguments[0])
        std::cout << "  " << mc_i18n("arguments") << ": " << v->minecraft_arguments << std::endl;
    mc_version_free(v);
    free(v);
}

static void cmd_manifest(int argc, char **argv) {
    int refresh = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--refresh") == 0) refresh = 1;
    }
    McManifest *m = fetch_manifest(refresh);
    if (!m) { std::cout << mc_i18n("error") << ": " << mc_i18n("no_matching") << std::endl; return; }
    std::cout << mc_i18n("manifest_updated") << ": " << m->count << " " << mc_i18n("version") << std::endl;
    std::cout << "Latest release: " << m->latest_release << std::endl;
    std::cout << "Latest snapshot: " << m->latest_snapshot << std::endl;
    free(m);
}

static void cmd_latest(void) {
    McManifest *m = fetch_manifest(0);
    if (!m) { std::cout << "Error: Failed to fetch version manifest" << std::endl; return; }
    std::cout << m->latest_release << std::endl;
    free(m);
}

int main(int argc, char **argv) {
    mc_console_init();
    mc_log_set_level(MC_LOG_INFO);

    const char *lang = nullptr;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--mirror") == 0) {
            g_mirror = argv[i + 1];
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2;
            break;
        }
    }
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--lang") == 0) {
            lang = argv[i + 1];
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2;
            break;
        }
    }
    if (lang) mc_i18n_set(lang);

    if (argc < 2) { print_help(); return 0; }
    if (strcmp(argv[1], "list") == 0) { cmd_list(argc, argv); return 0; }
    if (strcmp(argv[1], "search") == 0) { cmd_search(argc, argv); return 0; }
    if (strcmp(argv[1], "info") == 0) { cmd_info(argc, argv); return 0; }
    if (strcmp(argv[1], "manifest") == 0) { cmd_manifest(argc, argv); return 0; }
    if (strcmp(argv[1], "latest") == 0) { cmd_latest(); return 0; }
    std::cout << mc_i18n("unknown_command") << ": " << argv[1] << "\n" << std::endl;
    print_help();
    return 1;
}
