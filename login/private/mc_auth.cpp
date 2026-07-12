#include "mc_auth.h"
#include "mc_json.h"
#include "mc_http.h"
#include "mc_log.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <QtCore/QUuid>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QTextStream>

static const char *DEFAULT_AUTH_URL = "https://authserver.mojang.com";

void mc_auth_init(McAuthSession *session) {
    memset(session, 0, sizeof(McAuthSession));
    strcpy(session->server_url, DEFAULT_AUTH_URL);
    strcpy(session->user_type, "mojang");
}

void mc_auth_set_server(McAuthSession *session, const char *url) {
    if (session && url)
        strncpy(session->server_url, url, sizeof(session->server_url) - 1);
}

void mc_auth_generate_client_token(McAuthSession *session) {
    if (!session) return;
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray ba = uuid.toUtf8();
    strncpy(session->client_token, ba.constData(), sizeof(session->client_token) - 1);
}

static int do_auth_request(McAuthSession *session, const char *endpoint,
    const char *json_body, char *out_access_token, size_t out_access_size,
    char *out_uuid, size_t out_uuid_size,
    char *out_name, size_t out_name_size)
{
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", session->server_url, endpoint);

    mc_debug("[auth] POST %s", url);
    mc_debug("[auth] request: %s", json_body);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 30000);

    McHttpResponse *resp = mc_http_post_json(&client, url, json_body);
    if (!resp) {
        snprintf(session->error, sizeof(session->error), "No response from server");
        return 0;
    }

    int ok = 0;
    if (resp->success && resp->data) {
        mc_debug("[auth] response: %s", resp->data);
        McJson *j = mc_json_parse(resp->data);
        if (j) {
            const char *err_msg = mc_json_get_string(j, "errorMessage", NULL);
            if (err_msg) {
                snprintf(session->error, sizeof(session->error), "%s", err_msg);
            } else {
                const char *at = mc_json_get_string(j, "accessToken", NULL);
                const char *ct = mc_json_get_string(j, "clientToken", NULL);
                if (at) strncpy(out_access_token, at, out_access_size - 1);
                if (ct) strncpy(session->client_token, ct, sizeof(session->client_token) - 1);

                McJson *profile = mc_json_get(j, "selectedProfile");
                if (profile) {
                    const char *pid = mc_json_get_string(profile, "id", NULL);
                    const char *pname = mc_json_get_string(profile, "name", NULL);
                    if (pid) strncpy(out_uuid, pid, out_uuid_size - 1);
                    if (pname) strncpy(out_name, pname, out_name_size - 1);
                }
                ok = 1;
            }
            mc_json_free(j);
        } else {
            snprintf(session->error, sizeof(session->error), "Failed to parse JSON response");
        }
    } else {
        if (resp->error[0])
            snprintf(session->error, sizeof(session->error), "%s", resp->error);
        else
            snprintf(session->error, sizeof(session->error), "HTTP %ld", resp->status_code);
    }

    mc_http_response_free(resp);
    return ok;
}

int mc_auth_authenticate(McAuthSession *session, const char *username, const char *password) {
    if (!session || !username || !password) return 0;
    session->is_authenticated = 0;
    session->error[0] = '\0';

    if (!session->client_token[0])
        mc_auth_generate_client_token(session);

    char body[2048];
    snprintf(body, sizeof(body),
        "{"
        "\"agent\":{\"name\":\"Minecraft\",\"version\":1},"
        "\"username\":\"%s\","
        "\"password\":\"%s\","
        "\"clientToken\":\"%s\""
        "}",
        username, password, session->client_token);

    char access[MC_AUTH_TOKEN_SIZE] = "";
    char uuid[MC_AUTH_UUID_SIZE] = "";
    char name[MC_AUTH_NAME_SIZE] = "";

    if (do_auth_request(session, "/authenticate", body,
                        access, sizeof(access),
                        uuid, sizeof(uuid),
                        name, sizeof(name)))
    {
        strncpy(session->access_token, access, sizeof(session->access_token) - 1);
        strncpy(session->uuid, uuid, sizeof(session->uuid) - 1);
        strncpy(session->name, name, sizeof(session->name) - 1);
        session->is_authenticated = 1;
        return 1;
    }
    return 0;
}

int mc_auth_refresh(McAuthSession *session) {
    if (!session || !session->access_token[0] || !session->client_token[0]) {
        if (session) snprintf(session->error, sizeof(session->error),
            "No existing session to refresh");
        return 0;
    }
    session->is_authenticated = 0;
    session->error[0] = '\0';

    char body[2048];
    snprintf(body, sizeof(body),
        "{"
        "\"accessToken\":\"%s\","
        "\"clientToken\":\"%s\""
        "}",
        session->access_token, session->client_token);

    char access[MC_AUTH_TOKEN_SIZE] = "";
    char uuid[MC_AUTH_UUID_SIZE] = "";
    char name[MC_AUTH_NAME_SIZE] = "";

    if (do_auth_request(session, "/refresh", body,
                        access, sizeof(access),
                        uuid, sizeof(uuid),
                        name, sizeof(name)))
    {
        strncpy(session->access_token, access, sizeof(session->access_token) - 1);
        if (uuid[0]) strncpy(session->uuid, uuid, sizeof(session->uuid) - 1);
        if (name[0]) strncpy(session->name, name, sizeof(session->name) - 1);
        session->is_authenticated = 1;
        return 1;
    }
    return 0;
}

int mc_auth_validate(McAuthSession *session) {
    if (!session || !session->access_token[0] || !session->client_token[0]) {
        if (session) snprintf(session->error, sizeof(session->error),
            "No session to validate");
        return 0;
    }
    session->error[0] = '\0';

    char url[1024];
    snprintf(url, sizeof(url), "%s/validate", session->server_url);

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"accessToken\":\"%s\",\"clientToken\":\"%s\"}",
        session->access_token, session->client_token);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);

    McHttpResponse *resp = mc_http_post_json(&client, url, body);
    if (!resp) {
        snprintf(session->error, sizeof(session->error), "No response");
        return 0;
    }

    int ok = 0;
    if (resp->success && resp->status_code == 204) {
        ok = 1;
    } else if (resp->data) {
        McJson *j = mc_json_parse(resp->data);
        if (j) {
            const char *err = mc_json_get_string(j, "errorMessage", NULL);
            if (err) snprintf(session->error, sizeof(session->error), "%s", err);
            mc_json_free(j);
        }
    } else {
        snprintf(session->error, sizeof(session->error), "HTTP %ld", resp->status_code);
    }

    mc_http_response_free(resp);
    return ok;
}

int mc_auth_signout(const char *username, const char *password) {
    if (!username || !password) return 0;

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"username\":\"%s\",\"password\":\"%s\"}",
        username, password);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);

    McHttpResponse *resp = mc_http_post_json(&client,
        "https://authserver.mojang.com/signout", body);
    int ok = (resp && resp->success && resp->status_code == 204) ? 1 : 0;
    mc_http_response_free(resp);
    return ok;
}

int mc_auth_invalidate(McAuthSession *session) {
    if (!session || !session->access_token[0] || !session->client_token[0]) return 0;

    char url[1024];
    snprintf(url, sizeof(url), "%s/invalidate", session->server_url);

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"accessToken\":\"%s\",\"clientToken\":\"%s\"}",
        session->access_token, session->client_token);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);

    McHttpResponse *resp = mc_http_post_json(&client, url, body);
    int ok = (resp && resp->success && resp->status_code == 204) ? 1 : 0;
    mc_http_response_free(resp);
    return ok;
}

int mc_auth_save(const McAuthSession *session, const char *path) {
    if (!session || !path) return 0;

    McJson *j = mc_json_create_object();
    if (!j) return 0;

    mc_json_add_string(j, "accessToken", session->access_token);
    mc_json_add_string(j, "clientToken", session->client_token);
    mc_json_add_string(j, "uuid", session->uuid);
    mc_json_add_string(j, "name", session->name);
    mc_json_add_string(j, "userType", session->user_type);
    mc_json_add_number(j, "isAuthenticated", (double)session->is_authenticated);
    mc_json_add_string(j, "serverUrl", session->server_url);
    mc_json_add_string(j, "msaRefreshToken", session->msa_refresh_token);

    char *json_str = mc_json_stringify(j);
    int ok = 0;
    if (json_str) {
        QFile file(QString::fromUtf8(path));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << json_str;
            file.close();
            ok = 1;
        }
        free(json_str);
    }
    mc_json_free(j);
    return ok;
}

int mc_auth_load(McAuthSession *session, const char *path) {
    if (!session || !path) return 0;
    mc_auth_init(session);

    if (!QFile::exists(QString::fromUtf8(path))) return 0;

    QString data;
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
    QTextStream in(&file);
    data = in.readAll();
    file.close();

    McJson *j = mc_json_parse(data.toUtf8().constData());
    if (!j) return 0;

    const char *s;
    if ((s = mc_json_get_string(j, "accessToken", NULL)) != NULL)
        strncpy(session->access_token, s, sizeof(session->access_token) - 1);
    if ((s = mc_json_get_string(j, "clientToken", NULL)) != NULL)
        strncpy(session->client_token, s, sizeof(session->client_token) - 1);
    if ((s = mc_json_get_string(j, "uuid", NULL)) != NULL)
        strncpy(session->uuid, s, sizeof(session->uuid) - 1);
    if ((s = mc_json_get_string(j, "name", NULL)) != NULL)
        strncpy(session->name, s, sizeof(session->name) - 1);
    if ((s = mc_json_get_string(j, "userType", NULL)) != NULL)
        strncpy(session->user_type, s, sizeof(session->user_type) - 1);
    if ((s = mc_json_get_string(j, "serverUrl", NULL)) != NULL)
        strncpy(session->server_url, s, sizeof(session->server_url) - 1);
    if ((s = mc_json_get_string(j, "msaRefreshToken", NULL)) != NULL)
        strncpy(session->msa_refresh_token, s, sizeof(session->msa_refresh_token) - 1);
    session->is_authenticated = (int)mc_json_get_number(j, "isAuthenticated", 0);

    mc_json_free(j);
    return session->is_authenticated;
}
