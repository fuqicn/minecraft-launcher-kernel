#include <mc_mod.h>
#include <mc_i18n.h>
#include <mc_log.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <iomanip>

static void print_help(void) {
    std::cout << "modver - " << mc_i18n("modver_desc") << std::endl;
    std::cout << mc_i18n("usage") << ": modver <project-id|slug|url> [" << mc_i18n("options") << "]\n" << std::endl;
    std::cout << mc_i18n("options") << ":" << std::endl;
    std::cout << "  --platform <curseforge|modrinth>  " << mc_i18n("mod_platform") << std::endl;
    std::cout << "  --mcver <version>                 " << mc_i18n("mod_mcver_filter") << std::endl;
    std::cout << "  --loader <loader>                 " << mc_i18n("mod_loader_filter") << std::endl;
    std::cout << "  --mirror <type>                   " << mc_i18n("mirror") << std::endl;
    std::cout << "  --lang <code>                     " << mc_i18n("lang_opt") << std::endl;
    std::cout << "  --apikey <key>                    " << mc_i18n("mod_apikey") << std::endl;
    std::cout << "\n" << mc_i18n("examples") << ":" << std::endl;
    std::cout << "  modver sodium" << std::endl;
    std::cout << "  modver sodium --mcver 1.20.1 --loader fabric" << std::endl;
    std::cout << "  modver 394468 --platform curseforge" << std::endl;
    std::cout << "  modver https://modrinth.com/mod/sodium" << std::endl;
    std::cout << "  modver https://www.curseforge.com/minecraft/mc-mods/sodium" << std::endl;
}

// auto-detect source from URL or string
// returns: MC_MOD_CURSEFORGE, MC_MOD_MODRINTH, or 0 if can't determine
static int detect_source(const char *input, int explicit_source) {
    if (explicit_source) return explicit_source;

    // check if it's a URL
    if (strstr(input, "curseforge.com")) return MC_MOD_CURSEFORGE;
    if (strstr(input, "modrinth.com")) return MC_MOD_MODRINTH;

    // check if it's numeric (CurseForge project ID)
    int is_numeric = 1;
    for (const char *p = input; *p; p++)
        if (*p < '0' || *p > '9') { is_numeric = 0; break; }
    if (is_numeric) return MC_MOD_CURSEFORGE;

    // default: try Modrinth (uses slugs)
    return MC_MOD_MODRINTH;
}

// extract project ID from URL
static const char *extract_id(const char *input, int source) {
    if (source == MC_MOD_CURSEFORGE) {
        // curseforge.com/minecraft/mc-mods/<slug-or-id>
        const char *p = strstr(input, "curseforge.com/minecraft/");
        if (p) {
            p = strchr(p + 28, '/');
            if (p) return p + 1;
        }
        // maybe it's just a number
        int is_numeric = 1;
        for (const char *q = input; *q; q++)
            if (*q < '0' || *q > '9') { is_numeric = 0; break; }
        if (is_numeric) return input;
        return input; // fallback: use as-is (CF slug lookup not supported by API)
    } else {
        // modrinth.com/mod/<slug> or /plugin/<slug> etc.
        const char *p = strstr(input, "modrinth.com/");
        if (p) {
            p = strchr(p + 14, '/');
            if (p) return p + 1;
        }
        return input; // use as slug
    }
}

int main(int argc, char **argv) {
    mc_console_init();
    mc_log_set_level(MC_LOG_INFO);

    if (argc < 2) { print_help(); return 0; }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) { print_help(); return 0; }

    const char *input = nullptr;
    const char *mc_version = nullptr;
    const char *loader = nullptr;
    const char *mirror = nullptr;
    const char *apikey = nullptr;
    int platform = 0; // 0 = auto-detect

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            mc_i18n_set(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
            mirror = argv[++i];
        } else if (strcmp(argv[i], "--apikey") == 0 && i + 1 < argc) {
            apikey = argv[++i];
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            if (strcmp(val, "curseforge") == 0) platform = MC_MOD_CURSEFORGE;
            else if (strcmp(val, "modrinth") == 0) platform = MC_MOD_MODRINTH;
        } else if (strcmp(argv[i], "--mcver") == 0 && i + 1 < argc) {
            mc_version = argv[++i];
        } else if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
            loader = argv[++i];
        } else if (argv[i][0] != '-') {
            input = argv[i];
        }
    }

    if (!input) { print_help(); return 1; }

    if (mirror) mc_mod_set_mirror(mirror);
    if (apikey) mc_mod_set_api_key(apikey);

    const char *env_key = getenv("CURSEFORGE_API_KEY");
    if (env_key && env_key[0] && !apikey)
        mc_mod_set_api_key(env_key);

    int source = detect_source(input, platform);
    const char *project_id = extract_id(input, source);

    // fetch project info
    McModProject proj;
    mc_mod_project_init(&proj);
    int ok = mc_mod_get_project(project_id, source, &proj);

    if (!ok) {
        // try the other source if auto-detect
        if (source == MC_MOD_MODRINTH) {
            source = MC_MOD_CURSEFORGE;
            ok = mc_mod_get_project(project_id, source, &proj);
        } else {
            source = MC_MOD_MODRINTH;
            ok = mc_mod_get_project(project_id, source, &proj);
        }
    }

    if (!ok) {
        std::cerr << mc_i18n("mod_not_found") << ": " << input << std::endl;
        return 1;
    }

    // print project info
    std::cout << mc_i18n("mod_name_label") << ": " << (proj.name ? proj.name : "?") << std::endl;
    std::cout << mc_i18n("mod_id") << ": " << (proj.id ? proj.id : "?") << std::endl;
    std::cout << mc_i18n("mod_slug") << ": " << (proj.slug ? proj.slug : "?") << std::endl;
    std::cout << mc_i18n("mod_source") << ": " << (source == MC_MOD_CURSEFORGE ? "CurseForge" : "Modrinth") << std::endl;
    std::cout << mc_i18n("mod_downloads") << ": " << proj.download_count << std::endl;
    if (proj.game_versions)
        std::cout << mc_i18n("mod_versions") << ": " << proj.game_versions << std::endl;
    if (proj.loaders)
        std::cout << mc_i18n("mod_loaders") << ": " << proj.loaders << std::endl;
    if (proj.description)
        std::cout << mc_i18n("mod_desc") << ": " << proj.description << std::endl;
    if (proj.website_url)
        std::cout << mc_i18n("mod_website") << ": " << proj.website_url << std::endl;

    std::cout << "\n--- " << mc_i18n("mod_versions_list") << " ---" << std::endl;

    // fetch versions
    McModFile files[200];
    int fcount = mc_mod_get_versions(proj.id, source, mc_version, loader, files, 200);

    if (fcount == 0) {
        std::cout << mc_i18n("mod_no_versions") << std::endl;
    } else {
        const char *h_version = mc_i18n("mod_ver_name");
        const char *h_type = mc_i18n("mod_ver_type");
        const char *h_date = mc_i18n("mod_ver_date");
        const char *h_mcver = mc_i18n("mod_versions");
        const char *h_loader_label = mc_i18n("mod_loaders");
        const char *h_size = mc_i18n("mod_ver_size");

        // filter headers
        std::cout << std::left
                  << std::setw(40) << h_version
                  << std::setw(10) << h_type
                  << std::setw(22) << h_date
                  << std::setw(8) << h_size
                  << "URL"
                  << std::endl;
        std::cout << std::setfill('-')
                  << std::setw(40) << ""
                  << std::setw(10) << ""
                  << std::setw(22) << ""
                  << std::setw(8) << ""
                  << "------------------"
                  << std::setfill(' ') << std::endl;

        for (int i = 0; i < fcount && i < 40; i++) {
            McModFile *f = &files[i];
            std::string vname = f->display_name ? f->display_name : (f->file_name ? f->file_name : "?");
            if (vname.length() > 39) vname = vname.substr(0, 36) + "...";

            std::cout << std::left
                      << std::setw(40) << vname
                      << std::setw(10) << (f->release_type ? f->release_type : "?")
                      << std::setw(22) << (f->release_date ? f->release_date : "")
                      << std::setw(8) << (f->size > 0 ? std::to_string(f->size / 1024) + "KB" : "")
                      << (f->download_url ? f->download_url : "")
                      << std::endl;

            // show MC versions and loaders on second line if available
            bool has_extra = (f->game_versions && f->game_versions[0]) ||
                             (f->loaders && f->loaders[0]);
            if (has_extra) {
                std::cout << "  ";
                if (f->game_versions && f->game_versions[0])
                    std::cout << h_mcver << ": " << f->game_versions;
                if (f->loaders && f->loaders[0]) {
                    if (f->game_versions && f->game_versions[0])
                        std::cout << " | ";
                    std::cout << h_loader_label << ": " << f->loaders;
                }
                std::cout << std::endl;
            }
        }

        if (fcount > 40)
            std::cout << "\n(" << mc_i18n("mod_more_files") << ": " << (fcount - 40) << ")" << std::endl;
    }

    mc_mod_project_free(&proj);
    for (int i = 0; i < fcount; i++)
        mc_mod_file_free(&files[i]);

    return 0;
}
