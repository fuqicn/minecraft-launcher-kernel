#include "mc_mod.h"
#include "mc_http.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QByteArray>
#include <QUrl>
#include <cstring>
#include <cstdlib>
#include <cstdio>

static char g_mirror[64] = "";

static const char *MR_BASE = "https://api.modrinth.com/v2";

// mirror mapping
static void apply_mirror(QString &url) {
    if (!g_mirror[0]) return;
    if (strcmp(g_mirror, "bmclapi") == 0 || strcmp(g_mirror, "mcimirror") == 0) {
        url.replace("api.modrinth.com", "mod.mcimirror.top/modrinth");
        url.replace("cdn.modrinth.com", "mod.mcimirror.top");
    }
}

static char *strdup_qstring(const QString &s) {
    if (s.isEmpty()) return nullptr;
    QByteArray ba = s.toUtf8();
    char *r = (char *)malloc(ba.size() + 1);
    if (r) {
        memcpy(r, ba.constData(), ba.size());
        r[ba.size()] = '\0';
    }
    return r;
}

static int safe_int(const QJsonObject &obj, const char *key) {
    return obj.value(key).toInt(0);
}

static QString safe_string(const QJsonObject &obj, const char *key) {
    return obj.value(key).toString();
}

static QStringList safe_string_array(const QJsonObject &obj, const char *key) {
    QStringList r;
    QJsonArray a = obj.value(key).toArray();
    for (auto v : a) r.append(v.toString());
    return r;
}

void mc_mod_project_init(McModProject *p) {
    memset(p, 0, sizeof(*p));
}

void mc_mod_project_free(McModProject *p) {
    if (!p) return;
    free(p->id); free(p->slug); free(p->name); free(p->description);
    free(p->logo_url); free(p->game_versions); free(p->loaders);
    free(p->website_url);
    memset(p, 0, sizeof(*p));
}

void mc_mod_file_init(McModFile *f) {
    memset(f, 0, sizeof(*f));
}

void mc_mod_file_free(McModFile *f) {
    if (!f) return;
    free(f->id); free(f->project_id); free(f->display_name);
    free(f->file_name); free(f->download_url); free(f->sha1);
    free(f->game_versions); free(f->loaders); free(f->release_type);
    free(f->release_date);
    for (int i = 0; i < f->dependency_count; i++) {
        free(f->dependencies[i].project_id);
        free(f->dependencies[i].version_id);
        free(f->dependencies[i].file_name);
        free(f->dependencies[i].dependency_type);
    }
    free(f->dependencies);
    memset(f, 0, sizeof(*f));
}

void mc_mod_set_mirror(const char *mirror) {
    if (mirror) {
        strncpy(g_mirror, mirror, sizeof(g_mirror) - 1);
        g_mirror[sizeof(g_mirror) - 1] = '\0';
    }
}

static McHttpResponse *http_get_json(const char *url) {
    McHttpClient client;
    mc_http_init(&client);
    return mc_http_get(&client, url);
}

// ---- Modrinth search ----
static int search_modrinth(const char *query, int limit, int sort,
                           McModProject *results, int max_results) {
    QString url = QString("%1/search?limit=%2&index=relevance")
        .arg(MR_BASE).arg(qMin(limit, 50));

    if (query && query[0])
        url += "&query=" + QUrl::toPercentEncoding(QString::fromUtf8(query));
    else
        url += "&query=";

    if (sort == MC_MOD_SORT_DOWNLOADS) url += "&index=downloads";
    else if (sort == MC_MOD_SORT_FOLLOWS) url += "&index=follows";
    else if (sort == MC_MOD_SORT_NEWEST) url += "&index=newest";
    else if (sort == MC_MOD_SORT_UPDATED) url += "&index=updated";

    QString qurl = url;
    apply_mirror(qurl);

    McHttpResponse *resp = http_get_json(qurl.toUtf8().constData());
    if (!resp || !resp->success || resp->status_code != 200) {
        if (resp) mc_http_response_free(resp);
        return 0;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(resp->data, (int)resp->data_len), &err);
    mc_http_response_free(resp);
    if (err.error != QJsonParseError::NoError) return 0;

    QJsonArray hits = doc.object().value("hits").toArray();
    int count = 0;
    for (auto hit : hits) {
        if (count >= max_results) break;
        QJsonObject obj = hit.toObject();
        McModProject *p = &results[count];
        mc_mod_project_init(p);
        p->id = strdup_qstring(safe_string(obj, "project_id"));
        if (!p->id) p->id = strdup_qstring(safe_string(obj, "id"));
        p->slug = strdup_qstring(safe_string(obj, "slug"));
        p->name = strdup_qstring(safe_string(obj, "title"));
        p->description = strdup_qstring(safe_string(obj, "description"));
        p->download_count = safe_int(obj, "downloads");
        p->source = MC_MOD_MODRINTH;
        p->logo_url = strdup_qstring(safe_string(obj, "icon_url"));

        QStringList vers = safe_string_array(obj, "versions");
        if (!vers.isEmpty())
            p->game_versions = strdup_qstring(vers.join(", "));

        QStringList loaders = safe_string_array(obj, "loaders");
        if (!loaders.isEmpty())
            p->loaders = strdup_qstring(loaders.join(", "));

        count++;
    }
    return count;
}

int mc_mod_search(const char *query, int source,
                  int limit, int sort,
                  McModProject *results, int max_results) {
    if (!results || max_results <= 0) return 0;

    if (source != MC_MOD_MODRINTH && source != MC_MOD_ANY)
        return 0;

    return search_modrinth(query, limit, sort, results, max_results);
}

int mc_mod_get_project(const char *project_id, int source,
                       McModProject *project) {
    (void)source;
    if (!project_id || !project) return 0;
    mc_mod_project_init(project);

    QString url = QString("%1/project/%2").arg(MR_BASE).arg(project_id);
    QString qurl = url;
    apply_mirror(qurl);

    McHttpResponse *resp = http_get_json(qurl.toUtf8().constData());
    if (!resp || !resp->success || resp->status_code != 200) {
        if (resp) mc_http_response_free(resp);
        return 0;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(resp->data, (int)resp->data_len), &err);
    mc_http_response_free(resp);
    if (err.error != QJsonParseError::NoError) return 0;

    QJsonObject obj = doc.object();
    project->id = strdup_qstring(safe_string(obj, "id"));
    project->slug = strdup_qstring(safe_string(obj, "slug"));
    project->name = strdup_qstring(safe_string(obj, "title"));
    project->description = strdup_qstring(safe_string(obj, "description"));
    project->download_count = safe_int(obj, "downloads");
    project->source = MC_MOD_MODRINTH;
    project->logo_url = strdup_qstring(safe_string(obj, "icon_url"));
    project->website_url = strdup_qstring(safe_string(obj, "website_url"));

    QStringList gv = safe_string_array(obj, "game_versions");
    if (!gv.isEmpty())
        project->game_versions = strdup_qstring(gv.join(", "));
    else {
        QStringList vers = safe_string_array(obj, "versions");
        if (!vers.isEmpty())
            project->game_versions = strdup_qstring(vers.join(", "));
    }

    QStringList loaders = safe_string_array(obj, "loaders");
    if (!loaders.isEmpty())
        project->loaders = strdup_qstring(loaders.join(", "));

    return 1;
}

int mc_mod_get_versions(const char *project_id, int source,
                        const char *mc_version, const char *loader,
                        McModFile *files, int max_files) {
    (void)source;
    if (!project_id || !files || max_files <= 0) return 0;

    QString url = QString("%1/project/%2/version").arg(MR_BASE).arg(project_id);
    QString qurl = url;
    apply_mirror(qurl);

    McHttpResponse *resp = http_get_json(qurl.toUtf8().constData());
    if (!resp || !resp->success || resp->status_code != 200) {
        if (resp) mc_http_response_free(resp);
        return 0;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(resp->data, (int)resp->data_len), &err);
    mc_http_response_free(resp);
    if (err.error != QJsonParseError::NoError) return 0;

    int count = 0;
    QJsonArray data = doc.array();
    for (auto item : data) {
        if (count >= max_files) break;
        QJsonObject obj = item.toObject();

        if (mc_version && mc_version[0]) {
            QJsonArray gv = obj.value("game_versions").toArray();
            bool match = false;
            for (auto v : gv) {
                if (v.toString() == QString::fromUtf8(mc_version)) {
                    match = true; break;
                }
            }
            if (!match) continue;
        }

        if (loader && loader[0]) {
            QJsonArray ld = obj.value("loaders").toArray();
            bool match = false;
            for (auto v : ld) {
                if (v.toString() == QString::fromUtf8(loader)) {
                    match = true; break;
                }
            }
            if (!match) continue;
        }

        McModFile *f = &files[count];
        mc_mod_file_init(f);
        f->id = strdup_qstring(safe_string(obj, "id"));
        f->project_id = strdup_qstring(QString::fromUtf8(project_id));
        f->display_name = strdup_qstring(safe_string(obj, "name"));
        f->release_type = strdup_qstring(safe_string(obj, "version_type"));
        f->release_date = strdup_qstring(safe_string(obj, "date_published"));

        QJsonArray fileArr = obj.value("files").toArray();
        if (!fileArr.isEmpty()) {
            QJsonObject fObj = fileArr.first().toObject();
            f->file_name = strdup_qstring(safe_string(fObj, "filename"));
            f->sha1 = strdup_qstring(safe_string(fObj.value("hashes").toObject(), "sha1"));
            f->size = (long long)safe_int(fObj, "size");
            f->download_url = strdup_qstring(safe_string(fObj, "url"));
        }

        QStringList loadersArr = safe_string_array(obj, "loaders");
        if (!loadersArr.isEmpty())
            f->loaders = strdup_qstring(loadersArr.join(", "));

        QJsonArray deps = obj.value("dependencies").toArray();
        if (!deps.isEmpty()) {
            f->dependency_count = deps.size();
            f->dependencies = (McModDependency *)malloc((size_t)deps.size() * sizeof(McModDependency));
            if (f->dependencies) {
                memset(f->dependencies, 0, (size_t)deps.size() * sizeof(McModDependency));
                for (int j = 0; j < deps.size(); j++) {
                    QJsonObject d = deps[j].toObject();
                    f->dependencies[j].project_id = strdup_qstring(safe_string(d, "project_id"));
                    f->dependencies[j].version_id = strdup_qstring(safe_string(d, "version_id"));
                    f->dependencies[j].file_name = strdup_qstring(safe_string(d, "file_name"));
                    f->dependencies[j].dependency_type = strdup_qstring(safe_string(d, "dependency_type"));
                }
            }
        }

        count++;
    }

    return count;
}
