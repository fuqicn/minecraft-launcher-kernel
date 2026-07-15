#include <mc_mod.h>
#include <mc_i18n.h>
#include <mc_log.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <iomanip>

static void print_help(void) {
    std::cout << "modsearch - " << mc_i18n("modsearch_desc") << std::endl;
    std::cout << mc_i18n("usage") << ": modsearch [query] [" << mc_i18n("options") << "]\n" << std::endl;
    std::cout << mc_i18n("options") << ":" << std::endl;
    std::cout << "  --platform <curseforge|modrinth|both>  " << mc_i18n("mod_platform") << " (default: both)" << std::endl;
    std::cout << "  --sort <downloads|relevance|follows|newest>  " << mc_i18n("mod_sort") << std::endl;
    std::cout << "  --limit <n>  " << mc_i18n("mod_limit") << " (default: 20)" << std::endl;
    std::cout << "  --mirror <type>  " << mc_i18n("mirror") << std::endl;
    std::cout << "  --lang <code>  " << mc_i18n("lang_opt") << std::endl;
    std::cout << "  --apikey <key>  " << mc_i18n("mod_apikey") << std::endl;
    std::cout << "\n" << mc_i18n("examples") << ":" << std::endl;
    std::cout << "  modsearch" << std::endl;
    std::cout << "  modsearch sodium" << std::endl;
    std::cout << "  modsearch sodium --platform modrinth" << std::endl;
    std::cout << "  modsearch --platform curseforge --sort downloads" << std::endl;
}

static const char *source_name(int source) {
    return source == MC_MOD_CURSEFORGE ? "CurseForge" : "Modrinth";
}

int main(int argc, char **argv) {
    mc_console_init();
    mc_log_set_level(MC_LOG_ERROR);

    const char *query = nullptr;
    const char *mirror = nullptr;
    const char *apikey = nullptr;
    int source = MC_MOD_ANY;
    int sort = MC_MOD_SORT_DOWNLOADS;
    int limit = 20;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(); return 0;
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            mc_i18n_set(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
            mirror = argv[++i];
        } else if (strcmp(argv[i], "--apikey") == 0 && i + 1 < argc) {
            apikey = argv[++i];
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            if (strcmp(val, "curseforge") == 0) source = MC_MOD_CURSEFORGE;
            else if (strcmp(val, "modrinth") == 0) source = MC_MOD_MODRINTH;
        } else if (strcmp(argv[i], "--sort") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            if (strcmp(val, "relevance") == 0) sort = MC_MOD_SORT_RELEVANCE;
            else if (strcmp(val, "downloads") == 0) sort = MC_MOD_SORT_DOWNLOADS;
            else if (strcmp(val, "follows") == 0) sort = MC_MOD_SORT_FOLLOWS;
            else if (strcmp(val, "newest") == 0) sort = MC_MOD_SORT_NEWEST;
            else if (strcmp(val, "updated") == 0) sort = MC_MOD_SORT_UPDATED;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
            if (limit < 1) limit = 1;
            if (limit > 100) limit = 100;
        } else if (argv[i][0] != '-') {
            query = argv[i];
        }
    }

    // when no query, default sort is downloads (popular mods)
    if (!query) {
        sort = MC_MOD_SORT_DOWNLOADS;
    } else if (sort == MC_MOD_SORT_DOWNLOADS) {
        sort = MC_MOD_SORT_RELEVANCE;
    }

    if (mirror) mc_mod_set_mirror(mirror);
    if (apikey) mc_mod_set_api_key(apikey);

    // read api key from env if not set
    const char *env_key = getenv("CURSEFORGE_API_KEY");
    if (env_key && env_key[0] && !apikey)
        mc_mod_set_api_key(env_key);

    // warn about missing CurseForge API key when CF platform is selected
    if (source == MC_MOD_CURSEFORGE || source == MC_MOD_ANY) {
        const char *key = getenv("CURSEFORGE_API_KEY");
        if ((!key || !key[0]) && (!apikey || !apikey[0]))
            std::cerr << mc_i18n("warning") << ": " << mc_i18n("mod_no_cf_key") << std::endl;
    }

    McModProject results[200];
    int count = mc_mod_search(query, source, limit, sort, results, 200);

    if (count == 0) {
        if (query)
            std::cout << mc_i18n("mod_none_query") << " '" << query << "'" << std::endl;
        else
            std::cout << mc_i18n("mod_none") << std::endl;
        return 1;
    }

    const char *heading_id = mc_i18n("mod_id");
    const char *heading_name = mc_i18n("mod_name");
    const char *heading_source = mc_i18n("mod_source");
    const char *heading_dl = mc_i18n("mod_downloads");
    const char *heading_ver = mc_i18n("mod_versions");

    std::cout << std::left
              << std::setw(28) << heading_id
              << std::setw(48) << heading_name
              << std::setw(14) << heading_source
              << std::setw(12) << heading_dl
              << heading_ver
              << std::endl;
    std::cout << std::setfill('-')
              << std::setw(28) << ""
              << std::setw(48) << ""
              << std::setw(14) << ""
              << std::setw(12) << ""
              << "----------------------"
              << std::setfill(' ') << std::endl;

    for (int i = 0; i < count; i++) {
        McModProject *p = &results[i];
        std::cout << std::left
                  << std::setw(28) << (p->id ? p->id : "")
                  << std::setw(48) << (p->name ? p->name : "")
                  << std::setw(14) << source_name(p->source)
                  << std::setw(12) << p->download_count
                  << (p->game_versions ? p->game_versions : "")
                  << std::endl;

        if (p->description && p->description[0]) {
            std::string desc = p->description;
            if (desc.length() > 90) desc = desc.substr(0, 87) + "...";
            std::cout << "  " << desc << std::endl;
        }
    }

    for (int i = 0; i < count; i++)
        mc_mod_project_free(&results[i]);

    return 0;
}
