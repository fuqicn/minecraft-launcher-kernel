#ifndef MC_MOD_H
#define MC_MOD_H

#include <stddef.h>

#define MC_MOD_MAX_RESULTS 100

typedef struct {
    char *id;
    char *slug;
    char *name;
    char *description;
    char *logo_url;
    int download_count;
    char *game_versions;
    char *loaders;
    int source;
    char *website_url;
} McModProject;

typedef struct {
    char *id;
    char *project_id;
    char *display_name;
    char *file_name;
    char *download_url;
    char *sha1;
    long long size;
    char *game_versions;
    char *loaders;
    char *release_type;
    char *release_date;
    int download_count;
} McModFile;

enum McModSource {
    MC_MOD_CURSEFORGE = 1,
    MC_MOD_MODRINTH = 2,
    MC_MOD_ANY = 3,
};

enum McModSort {
    MC_MOD_SORT_RELEVANCE = 0,
    MC_MOD_SORT_DOWNLOADS = 1,
    MC_MOD_SORT_FOLLOWS = 2,
    MC_MOD_SORT_NEWEST = 3,
    MC_MOD_SORT_UPDATED = 4,
};

void mc_mod_project_init(McModProject *p);
void mc_mod_project_free(McModProject *p);
void mc_mod_file_init(McModFile *f);
void mc_mod_file_free(McModFile *f);

void mc_mod_set_api_key(const char *key);
void mc_mod_set_mirror(const char *mirror);

int mc_mod_search(const char *query, int source,
                  int limit, int sort,
                  McModProject *results, int max_results);

int mc_mod_get_project(const char *project_id, int source,
                       McModProject *project);

int mc_mod_get_versions(const char *project_id, int source,
                        const char *mc_version, const char *loader,
                        McModFile *files, int max_files);

int mc_mod_search_both(const char *query, int limit, int sort,
                       McModProject *results, int max_results);

#endif
