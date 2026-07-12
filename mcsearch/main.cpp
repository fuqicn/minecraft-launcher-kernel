#include <mc_manifest.h>
#include <mc_i18n.h>
#include <mc_path.h>
#include <mc_log.h>
#include <iostream>
#include <cstring>
#include <cstdlib>

static void print_help() {
    std::cout << "mcsearch - " << mc_i18n("mcsearch_desc") << std::endl
              << mc_i18n("usage") << ": mcsearch [" << mc_i18n("options") << "]" << std::endl
              << std::endl
              << mc_i18n("options") << ":" << std::endl
              << "  --type <release|snapshot|old_beta|old_alpha|all>  " << mc_i18n("filter_type") << std::endl
              << "  --mirror <mojang|bmclapi|mcbbs>  " << mc_i18n("mirror_source") << std::endl
              << "  --lang <code>  " << mc_i18n("lang_opt") << std::endl
              << std::endl
              << mc_i18n("examples") << ":" << std::endl
              << "  mcsearch" << std::endl
              << "  mcsearch --type release" << std::endl
              << "  mcsearch --type all --mirror bmclapi" << std::endl;
}

int main(int argc, char **argv) {
    mc_console_init();
    mc_log_set_level(MC_LOG_ERROR);

    const char *mirror = "mojang";
    const char *filter_type = "all";

    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--lang") == 0) { mc_i18n_set(argv[i + 1]); break; }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            filter_type = argv[++i];
        } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
            mirror = argv[++i];
        }
    }

    McManifest m;
    if (!mc_manifest_load_cache(&m)) {
        if (!mc_manifest_fetch_mirror(&m, 1, mirror)) {
            std::cerr << mc_i18n("err_fetch_manifest") << std::endl;
            return 1;
        }
        mc_manifest_save_cache(&m);
    }

    for (int i = 0; i < m.count; i++) {
        bool match = false;
        if (strcmp(filter_type, "all") == 0) {
            match = true;
        } else if (strcmp(m.entries[i].type, filter_type) == 0) {
            match = true;
        }
        if (match) {
            char release_time[64] = "", modified_time[64] = "";
            if (m.entries[i].release_time > 0) {
                time_t rt = static_cast<time_t>(m.entries[i].release_time);
                struct tm *tm = gmtime(&rt);
                if (tm) strftime(release_time, sizeof(release_time), "%Y-%m-%dT%H:%M:%SZ", tm);
            }
            if (m.entries[i].modified_time > 0) {
                time_t mt = static_cast<time_t>(m.entries[i].modified_time);
                struct tm *tm = gmtime(&mt);
                if (tm) strftime(modified_time, sizeof(modified_time), "%Y-%m-%dT%H:%M:%SZ", tm);
            }
            std::cout << m.entries[i].type << "|" << m.entries[i].id << "|"
                      << release_time << "|" << modified_time << std::endl;
        }
    }

    return 0;
}
