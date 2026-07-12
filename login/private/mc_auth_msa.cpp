#include "mc_auth_msa.h"
#include "mc_json.h"
#include "mc_http.h"
#include "mc_log.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <QtCore/QUrl>
#include <QtCore/QProcess>
#include <QtCore/QElapsedTimer>
#ifdef _WIN32
#include <windows.h>
#endif

#define MSA_CLIENT_ID "00000000402B5328"

static void url_encode(const char *src, char *dst, size_t dst_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 4; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
}

static void copy_to_clipboard(const char *text) {
#ifdef _WIN32
    if (!text || !text[0]) return;
    size_t len = strlen(text);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (!h) return;
    memcpy(GlobalLock(h), text, len + 1);
    GlobalUnlock(h);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, h);
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
#else
    (void)text;
#endif
}

static void open_url(const QUrl &url) {
    QString urlStr = url.toString();
#ifdef _WIN32
    QProcess::startDetached("cmd.exe", QStringList() << "/c" << "start" << "" << urlStr);
#elif defined(__APPLE__)
    QProcess::startDetached("open", QStringList() << urlStr);
#else
    QProcess::startDetached("xdg-open", QStringList() << urlStr);
#endif
}

static void format_uuid(const char *raw, char *out, size_t out_size) {
    size_t len = strlen(raw);
    if (len >= 32 && out_size >= 37) {
        memcpy(out, raw, 8);
        out[8] = '-';
        memcpy(out + 9, raw + 8, 4);
        out[13] = '-';
        memcpy(out + 14, raw + 12, 4);
        out[18] = '-';
        memcpy(out + 19, raw + 16, 4);
        out[23] = '-';
        memcpy(out + 24, raw + 20, 12);
        out[36] = '\0';
    } else {
        strncpy(out, raw, out_size - 1);
    }
}

static int http_ok(McHttpResponse *resp) {
    return resp && resp->data && resp->status_code >= 200 && resp->status_code < 300;
}

static McHttpResponse *post_form(const char *url, const char *form_body) {
    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 30000);
    return mc_http_post(&client, url, "application/x-www-form-urlencoded",
                        (const unsigned char *)form_body, strlen(form_body));
}

static int do_device_code_flow(char *msa_token, size_t msa_size,
                                char *refresh_token, size_t refresh_size) {
    // Step 1: Request device code (using legacy login.live.com endpoint with Title ID)
    char body[1024];
    snprintf(body, sizeof(body),
        "client_id=%s&scope=XboxLive.signin%%20offline_access&response_type=device_code",
        MSA_CLIENT_ID);

    McHttpResponse *resp = post_form(
        "https://login.live.com/oauth20_connect.srf", body);
    mc_debug("[MSA] Device code POST body: %s", body);

    if (!resp) {
        mc_error("[MSA] Device code request failed (no response)");
        return 0;
    }
    mc_debug("[MSA] Device code response: status=%ld, success=%d, error=[%s]",
        resp->status_code, resp->success, resp->error);
    if (resp->data) mc_debug("[MSA] Device code body: %.300s", resp->data);
    if (!http_ok(resp)) {
        mc_error("[MSA] Device code request failed: status=%ld, error=%s, body=%s",
            resp->status_code, resp->error[0] ? resp->error : "none",
            resp->data ? resp->data : "(no body)");
        mc_http_response_free(resp);
        return 0;
    }

    McJson *j = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!j) { mc_error("[MSA] Failed to parse device code response"); return 0; }

    const char *user_code = mc_json_get_string(j, "user_code", NULL);
    const char *device_code = mc_json_get_string(j, "device_code", NULL);
    const char *verif_uri = mc_json_get_string(j, "verification_uri", NULL);
    int interval = mc_json_get_int(j, "interval", 5);

    if (!user_code || !device_code || !verif_uri) {
        const char *err = mc_json_get_string(j, "error", NULL);
        mc_error("[MSA] Device code error: %s", err ? err : "incomplete response");
        mc_json_free(j);
        return 0;
    }

    // Copy before freeing JSON (strings point into JSON tree)
    char saved_device_code[2048];
    char saved_user_code[64];
    char saved_verif_uri[512];
    strncpy(saved_device_code, device_code, sizeof(saved_device_code) - 1);
    strncpy(saved_user_code, user_code, sizeof(saved_user_code) - 1);
    strncpy(saved_verif_uri, verif_uri, sizeof(saved_verif_uri) - 1);

    // Step 2: Display to user and open browser
    mc_info("");
    mc_info("========================================");
    mc_info(" Microsoft Login");
    mc_info("========================================");
    mc_info(" 1. Open: %s", saved_verif_uri);
    mc_info(" 2. Enter code: %s (copied to clipboard)", saved_user_code);
    mc_info("========================================");
    mc_info("");

    copy_to_clipboard(saved_user_code);
    QUrl verifUrl(QString::fromUtf8(saved_verif_uri));
    open_url(verifUrl);

    // Now safe to free JSON
    mc_json_free(j);

    // Step 3: Poll for token
    mc_info("[MSA] Waiting for authentication...");

    QElapsedTimer timer;
    timer.start();
    int max_wait_sec = 300; // 5 minutes

    while (timer.elapsed() < max_wait_sec * 1000) {
        mc_http_sleep(interval * 1000);

        char poll_body[2048];
        char enc_dev_code[2048];
        url_encode(saved_device_code, enc_dev_code, sizeof(enc_dev_code));
        snprintf(poll_body, sizeof(poll_body),
            "grant_type=device_code&client_id=%s&device_code=%s",
            MSA_CLIENT_ID, enc_dev_code);

        McHttpResponse *poll_resp = post_form(
            "https://login.live.com/oauth20_token.srf", poll_body);

        if (!poll_resp || !poll_resp->data) {
            if (poll_resp) mc_http_response_free(poll_resp);
            mc_info("[MSA] Poll error, retrying...");
            continue;
        }

        McJson *pj = mc_json_parse(poll_resp->data);
        mc_http_response_free(poll_resp);
        if (!pj) continue;

        const char *err = mc_json_get_string(pj, "error", NULL);
        if (err) {
            if (strcmp(err, "authorization_pending") == 0) {
                mc_json_free(pj);
                continue;
            }
            if (strcmp(err, "slow_down") == 0) {
                interval += 5;
                mc_json_free(pj);
                continue;
            }
            mc_error("[MSA] Auth error: %s", err);
            mc_json_free(pj);
            return 0;
        }

        const char *at = mc_json_get_string(pj, "access_token", NULL);
        const char *rt = mc_json_get_string(pj, "refresh_token", NULL);
        if (!at) {
            mc_error("[MSA] No access_token in device code response");
            mc_json_free(pj);
            return 0;
        }

        strncpy(msa_token, at, msa_size - 1);
        if (rt) strncpy(refresh_token, rt, refresh_size - 1);
        mc_json_free(pj);
        return 1;
    }

    mc_error("[MSA] Device code auth timed out");
    return 0;
}

static char *xbl_authenticate(const char *msa_token, char *uhs, size_t uhs_size) {
    mc_debug("[MSA] XBL msa_token starts with: %.100s", msa_token ? msa_token : "(null)");
    char body[4096];
    int n = snprintf(body, sizeof(body),
        "{"
        "\"Properties\":{"
        "\"AuthMethod\":\"RPS\","
        "\"SiteName\":\"user.auth.xboxlive.com\","
        "\"RpsTicket\":\"d=%s\""
        "},"
        "\"RelyingParty\":\"http://auth.xboxlive.com\","
        "\"TokenType\":\"JWT\""
        "}", msa_token ? msa_token : "");
    mc_debug("[MSA] XBL request body length: %d", n);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);
    McHttpResponse *resp = mc_http_post_json(&client,
        "https://user.auth.xboxlive.com/user/authenticate", body);
    if (!resp) {
        mc_error("[MSA] XBL authentication failed (no response)");
        return nullptr;
    }
    mc_debug("[MSA] XBL response: status=%ld, success=%d, error=[%s], data_len=%zu",
        resp->status_code, resp->success, resp->error, resp->data_len);
    if (resp->data) {
        mc_debug("[MSA] XBL body: %.200s", resp->data);
    }
    if (!http_ok(resp)) {
        mc_error("[MSA] XBL authentication failed: status=%ld, error=%s, body=%s",
            resp->status_code, resp->error[0] ? resp->error : "empty error",
            resp->data ? resp->data : "(no body)");
        mc_http_response_free(resp);
        return nullptr;
    }

    McJson *j = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!j) { mc_error("[MSA] Failed to parse XBL response"); return nullptr; }

    const char *token = mc_json_get_string(j, "Token", NULL);
    if (!token) { mc_error("[MSA] No XBL token"); mc_json_free(j); return nullptr; }

    McJson *claims = mc_json_get(j, "DisplayClaims");
    McJson *xui = claims ? mc_json_get(claims, "xui") : nullptr;
    if (xui && xui->type == MC_JSON_ARRAY) {
        McJson *first = mc_json_get_array_item(xui, 0);
        if (first) {
            const char *uhs_val = mc_json_get_string(first, "uhs", NULL);
            if (uhs_val) strncpy(uhs, uhs_val, uhs_size - 1);
        }
    }

    char *result = strdup(token);
    mc_json_free(j);
    return result;
}

static char *xsts_authenticate(const char *xbl_token) {
    char body[4096];
    snprintf(body, sizeof(body),
        "{"
        "\"Properties\":{"
        "\"SandboxId\":\"RETAIL\","
        "\"UserTokens\":[\"%s\"]"
        "},"
        "\"RelyingParty\":\"rp://api.minecraftservices.com/\","
        "\"TokenType\":\"JWT\""
        "}", xbl_token);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);
    McHttpResponse *resp = mc_http_post_json(&client,
        "https://xsts.auth.xboxlive.com/xsts/authorize", body);
    if (!http_ok(resp)) {
        mc_error("[MSA] XSTS authentication failed: status=%ld, body=%s",
            resp ? resp->status_code : 0,
            (resp && resp->data) ? resp->data : "(no response)");
        if (resp) mc_http_response_free(resp);
        return nullptr;
    }

    McJson *j = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!j) { mc_error("[MSA] Failed to parse XSTS response"); return nullptr; }

    const char *err = mc_json_get_string(j, "XErr", NULL);
    if (err) {
        mc_error("[MSA] XSTS error XErr=%s", err);
        mc_json_free(j);
        return nullptr;
    }

    const char *token = mc_json_get_string(j, "Token", NULL);
    if (!token) { mc_error("[MSA] No XSTS token"); mc_json_free(j); return nullptr; }

    char *result = strdup(token);
    mc_json_free(j);
    return result;
}

static char *mc_login(const char *uhs, const char *xsts_token) {
    char identity[4096];
    snprintf(identity, sizeof(identity), "XBL3.0 x=%s;%s", uhs, xsts_token);

    char body[4096];
    snprintf(body, sizeof(body),
        "{\"identityToken\":\"%s\"}", identity);

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);
    McHttpResponse *resp = mc_http_post_json(&client,
        "https://api.minecraftservices.com/authentication/login_with_xbox", body);
    if (!http_ok(resp)) {
        mc_error("[MSA] Minecraft login failed: status=%ld, body=%s",
            resp ? resp->status_code : 0,
            (resp && resp->data) ? resp->data : "(no response)");
        if (resp) mc_http_response_free(resp);
        return nullptr;
    }

    mc_debug("[MSA] MC login response: %.800s", resp->data);

    McJson *j = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!j) { mc_error("[MSA] Failed to parse Minecraft login response"); return nullptr; }

    const char *token_type = mc_json_get_string(j, "token_type", NULL);
    mc_debug("[MSA] MC login token_type: %s", token_type ? token_type : "(null)");

    const char *token = mc_json_get_string(j, "access_token", NULL);
    if (!token) { mc_error("[MSA] No Minecraft access_token"); mc_json_free(j); return nullptr; }

    char *result = strdup(token);
    mc_json_free(j);
    return result;
}

static void jwt_decode_profile(const char *jwt, char *uuid, size_t uuid_size,
                                char *name, size_t name_size) {
    uuid[0] = '\0';
    name[0] = '\0';
    if (!jwt) return;

    // Find the second dot (between payload and signature)
    const char *first_dot = strchr(jwt, '.');
    if (!first_dot) return;
    const char *second_dot = strchr(first_dot + 1, '.');
    if (!second_dot) return;

    // Extract payload (between first and second dot)
    size_t payload_len = second_dot - first_dot - 1;
    if (payload_len == 0) return;

    // Copy and convert base64url to base64
    char *b64 = (char *)malloc(payload_len + 4);
    if (!b64) return;
    for (size_t i = 0; i < payload_len; i++) {
        char c = first_dot[1 + i];
        b64[i] = (c == '-') ? '+' : (c == '_') ? '/' : c;
    }
    // Add padding
    size_t padded = payload_len;
    while (padded % 4) b64[padded++] = '=';
    b64[padded] = '\0';

    QByteArray decoded = QByteArray::fromBase64(QByteArray(b64, padded));
    free(b64);

    if (decoded.isEmpty()) return;

    McJson *j = mc_json_parse(decoded.constData());
    if (!j) return;

    // Extract UUID from profiles.mc
    McJson *profiles = mc_json_get(j, "profiles");
    if (profiles) {
        const char *mc_uuid = mc_json_get_string(profiles, "mc", NULL);
        if (mc_uuid) {
            // UUID from JWT is already formatted with hyphens
            strncpy(uuid, mc_uuid, uuid_size - 1);
        }
    }

    // Extract username from pfd[0].name
    if (!name[0]) {
        McJson *pfd = mc_json_get(j, "pfd");
        if (pfd && pfd->type == MC_JSON_ARRAY) {
            McJson *first = mc_json_get_array_item(pfd, 0);
            if (first) {
                const char *n = mc_json_get_string(first, "name", NULL);
                if (n) strncpy(name, n, name_size - 1);
            }
        }
    }

    mc_json_free(j);
}

static int get_mc_profile(const char *mc_token, char *uuid, size_t uuid_size,
                          char *name, size_t name_size) {
    char auth_header[1024];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", mc_token);
    mc_debug("[MSA] Profile header: %.150s...", auth_header);
    const char *headers[] = { auth_header };

    McHttpClient client;
    mc_http_init(&client);
    mc_http_set_timeout(&client, 15000);
    McHttpResponse *resp = mc_http_get_with_headers(&client,
        "https://api.minecraftservices.com/minecraft/profile", headers, 1);
    mc_debug("[MSA] Profile resp: status=%ld, success=%d, error=[%s]",
        resp ? resp->status_code : 0, resp ? resp->success : 0,
        resp ? resp->error : "null");
    if (!http_ok(resp)) {
        mc_error("[MSA] Failed to get Minecraft profile: status=%ld, body=%s",
            resp ? resp->status_code : 0,
            (resp && resp->data) ? resp->data : "(no response)");
        if (resp && resp->data) {
            McJson *pj = mc_json_parse(resp->data);
            if (pj) {
                mc_debug("[MSA] Profile error: %s, errorMessage: %s",
                    mc_json_get_string(pj, "error", "(none)"),
                    mc_json_get_string(pj, "errorMessage", "(none)"));
                mc_json_free(pj);
            }
        }
        if (resp) mc_http_response_free(resp);
        // Fallback: decode profile from JWT
        mc_info("[MSA] Falling back to JWT profile decode...");
        jwt_decode_profile(mc_token, uuid, uuid_size, name, name_size);
        if (uuid[0] && name[0]) {
            mc_info("[MSA] Profile from JWT: %s (%s)", name, uuid);
            return 1;
        }
        return 0;
    }

    McJson *j = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!j) { mc_error("[MSA] Failed to parse profile response"); return 0; }

    const char *err = mc_json_get_string(j, "error", NULL);
    if (err) {
        mc_error("[MSA] Profile error: %s", err);
        mc_json_free(j);
        return 0;
    }

    const char *raw_id = mc_json_get_string(j, "id", NULL);
    const char *nm = mc_json_get_string(j, "name", NULL);
    if (!raw_id || !nm) {
        mc_error("[MSA] Incomplete profile data");
        mc_json_free(j);
        return 0;
    }

    // Format UUID with hyphens
    format_uuid(raw_id, uuid, uuid_size);
    strncpy(name, nm, name_size - 1);
    mc_json_free(j);
    return 1;
}

int mc_auth_msa_login(McAuthSession *session) {
    if (!session) return 0;

    McAuthSession tmp;
    mc_auth_init(&tmp);

    char msa_token[2048] = "";
    char refresh_token[2048] = "";

    if (!do_device_code_flow(msa_token, sizeof(msa_token),
                            refresh_token, sizeof(refresh_token)))
        return 0;

    if (refresh_token[0])
        strncpy(tmp.msa_refresh_token, refresh_token,
                sizeof(tmp.msa_refresh_token) - 1);

    char uhs[256] = "";
    char *xbl_token = xbl_authenticate(msa_token, uhs, sizeof(uhs));
    if (!xbl_token) return 0;
    mc_info("[MSA] XBL authenticated");

    char *xsts_token = xsts_authenticate(xbl_token);
    free(xbl_token);
    if (!xsts_token) return 0;
    mc_info("[MSA] XSTS authorized");

    char *mc_token = mc_login(uhs, xsts_token);
    free(xsts_token);
    if (!mc_token) return 0;
    mc_info("[MSA] Minecraft authenticated");

    strncpy(tmp.access_token, mc_token, sizeof(tmp.access_token) - 1);
    strncpy(tmp.user_type, "msa", sizeof(tmp.user_type) - 1);

    if (!get_mc_profile(mc_token, tmp.uuid, sizeof(tmp.uuid),
                        tmp.name, sizeof(tmp.name))) {
        mc_error("[MSA] Failed to get Minecraft profile");
        free(mc_token);
        return 0;
    }
    free(mc_token);

    mc_info("[MSA] Logged in as: %s (%s)", tmp.name, tmp.uuid);
    tmp.is_authenticated = 1;
    *session = tmp;
    return 1;
}

int mc_auth_msa_refresh(McAuthSession *session) {
    if (!session || !session->msa_refresh_token[0]) {
        if (session) mc_error("[MSA] No refresh token available");
        return 0;
    }

    char body[2048];
    snprintf(body, sizeof(body),
        "client_id=%s&refresh_token=%s&grant_type=refresh_token"
        "&scope=XboxLive.signin+offline_access",
        MSA_CLIENT_ID, session->msa_refresh_token);

    McHttpResponse *resp = post_form("https://login.live.com/oauth20_token.srf", body);
    if (!http_ok(resp)) {
        mc_error("[MSA] Refresh failed: status=%ld, body=%s",
            resp ? resp->status_code : 0,
            (resp && resp->data) ? resp->data : "(no response)");
        if (resp) mc_http_response_free(resp);
        return 0;
    }

    McJson *j = mc_json_parse(resp->data);
    mc_http_response_free(resp);
    if (!j) { mc_error("[MSA] Failed to parse refresh response"); return 0; }

    const char *err = mc_json_get_string(j, "error", NULL);
    if (err) {
        mc_error("[MSA] Refresh error: %s", err);
        mc_json_free(j);
        return 0;
    }

    const char *msa_token = mc_json_get_string(j, "access_token", NULL);
    const char *new_refresh = mc_json_get_string(j, "refresh_token", NULL);
    if (!msa_token) { mc_error("[MSA] No access_token in refresh"); mc_json_free(j); return 0; }

    if (new_refresh)
        strncpy(session->msa_refresh_token, new_refresh,
                sizeof(session->msa_refresh_token) - 1);

    mc_json_free(j);

    char uhs[256] = "";
    char *xbl_token = xbl_authenticate(msa_token, uhs, sizeof(uhs));
    if (!xbl_token) return 0;

    char *xsts_token = xsts_authenticate(xbl_token);
    free(xbl_token);
    if (!xsts_token) return 0;

    char *mc_token = mc_login(uhs, xsts_token);
    free(xsts_token);
    if (!mc_token) return 0;

    strncpy(session->access_token, mc_token, sizeof(session->access_token) - 1);

    char uuid[MC_AUTH_UUID_SIZE] = "";
    char name[MC_AUTH_NAME_SIZE] = "";
    int profile_ok = get_mc_profile(mc_token, uuid, sizeof(uuid), name, sizeof(name));
    free(mc_token);

    if (profile_ok) {
        strncpy(session->uuid, uuid, sizeof(session->uuid) - 1);
        strncpy(session->name, name, sizeof(session->name) - 1);
    }

    session->is_authenticated = 1;
    mc_info("[MSA] Session refreshed for %s", session->name);
    return 1;
}
