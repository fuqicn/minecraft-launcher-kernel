#include "mc_json.h"
#include "mc_str.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cerrno>
#include <sstream>
#include <iomanip>

static McJson *parse_value(const char **pos);
static void skip_ws(const char **pos) { while (**pos && (**pos == ' ' || **pos == '\t' || **pos == '\n' || **pos == '\r')) (*pos)++; }

static McJson *create_node(McJsonType type) {
    McJson *j = (McJson *)std::calloc(1, sizeof(McJson));
    if (j) j->type = type;
    return j;
}

static char *parse_string(const char **pos) {
    if (**pos != '"') return NULL;
    (*pos)++;
    size_t cap = 64, len = 0;
    char *s = (char *)std::malloc(cap);
    if (!s) return NULL;
    while (**pos && **pos != '"') {
        if (**pos == '\\') {
            (*pos)++;
            char c = '\0';
            switch (**pos) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    char hex[5] = {0};
                    for (int i = 0; i < 4; i++) { (*pos)++; hex[i] = **pos; }
                    unsigned int cp = (unsigned int)std::strtoul(hex, NULL, 16);
                    if (cp < 0x80) c = (char)cp;
                    else if (cp < 0x800) {
                        s[len++] = (char)(0xC0 | (cp >> 6));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else {
                        s[len++] = (char)(0xE0 | (cp >> 12));
                        s[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        c = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: c = **pos; break;
            }
            if (c) {
                if (len + 1 >= cap) { cap *= 2; s = (char *)std::realloc(s, cap); }
                s[len++] = c;
            }
            if (**pos) (*pos)++;
        } else {
            if (len + 1 >= cap) { cap *= 2; s = (char *)std::realloc(s, cap); }
            s[len++] = **pos;
            (*pos)++;
        }
    }
    if (**pos == '"') (*pos)++;
    s[len] = '\0';
    return s;
}

static McJson *parse_object(const char **pos) {
    McJson *obj = create_node(MC_JSON_OBJECT);
    if (!obj) return NULL;
    (*pos)++;
    skip_ws(pos);
    if (**pos == '}') { (*pos)++; return obj; }
    McJson *tail = NULL;
    while (1) {
        skip_ws(pos);
        char *key = parse_string(pos);
        if (!key) break;
        skip_ws(pos);
        if (**pos != ':') { std::free(key); break; }
        (*pos)++;
        skip_ws(pos);
        McJson *val = parse_value(pos);
        if (!val) { std::free(key); break; }
        val->key = key;
        if (!obj->child) { obj->child = val; tail = val; }
        else { tail->next = val; val->prev = tail; tail = val; }
        skip_ws(pos);
        if (**pos == ',') { (*pos)++; continue; }
        if (**pos == '}') { (*pos)++; break; }
        break;
    }
    return obj;
}

static McJson *parse_array(const char **pos) {
    McJson *arr = create_node(MC_JSON_ARRAY);
    if (!arr) return NULL;
    (*pos)++;
    skip_ws(pos);
    if (**pos == ']') { (*pos)++; return arr; }
    McJson *tail = NULL;
    while (1) {
        McJson *val = parse_value(pos);
        if (!val) break;
        if (!arr->child) { arr->child = val; tail = val; }
        else { tail->next = val; val->prev = tail; tail = val; }
        skip_ws(pos);
        if (**pos == ',') { (*pos)++; continue; }
        if (**pos == ']') { (*pos)++; break; }
        break;
    }
    return arr;
}

static McJson *parse_number(const char **pos) {
    McJson *j = create_node(MC_JSON_NUMBER);
    if (!j) return NULL;
    const char *start = *pos;
    if (**pos == '-') (*pos)++;
    while (**pos >= '0' && **pos <= '9') (*pos)++;
    if (**pos == '.') {
        (*pos)++;
        while (**pos >= '0' && **pos <= '9') (*pos)++;
    }
    if (**pos == 'e' || **pos == 'E') {
        (*pos)++;
        if (**pos == '+' || **pos == '-') (*pos)++;
        while (**pos >= '0' && **pos <= '9') (*pos)++;
    }
    size_t len = *pos - start;
    char *buf = (char *)std::malloc(len + 1);
    if (!buf) { std::free(j); return NULL; }
    std::memcpy(buf, start, len);
    buf[len] = '\0';
    j->number_value = std::strtod(buf, NULL);
    j->string_value = buf;
    return j;
}

static McJson *parse_value(const char **pos) {
    skip_ws(pos);
    if (**pos == '\0') return NULL;
    if (**pos == '"') {
        char *s = parse_string(pos);
        if (!s) return NULL;
        McJson *j = create_node(MC_JSON_STRING);
        if (!j) { std::free(s); return NULL; }
        j->string_value = s;
        return j;
    }
    if (**pos == '{') return parse_object(pos);
    if (**pos == '[') return parse_array(pos);
    if (**pos == '-' || (**pos >= '0' && **pos <= '9')) return parse_number(pos);
    if (std::strncmp(*pos, "true", 4) == 0) { McJson *j = create_node(MC_JSON_BOOL); if (j) j->bool_value = 1; *pos += 4; return j; }
    if (std::strncmp(*pos, "false", 5) == 0) { McJson *j = create_node(MC_JSON_BOOL); if (j) j->bool_value = 0; *pos += 5; return j; }
    if (std::strncmp(*pos, "null", 4) == 0) { *pos += 4; return create_node(MC_JSON_NULL); }
    return NULL;
}

McJson *mc_json_parse(const char *str) {
    if (!str) return NULL;
    const char *pos = str;
    McJson *j = parse_value(&pos);
    return j;
}

McJson *mc_json_parse_file(const char *path) {
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return NULL;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) { std::fclose(f); return NULL; }
    char *buf = (char *)std::malloc((size_t)len + 1);
    if (!buf) { std::fclose(f); return NULL; }
    std::fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    std::fclose(f);
    McJson *j = mc_json_parse(buf);
    std::free(buf);
    return j;
}

static void append_str(char **buf, size_t *cap, size_t *pos, const char *s) {
    size_t slen = std::strlen(s);
    while (*pos + slen + 1 >= *cap) { *cap *= 2; *buf = (char *)std::realloc(*buf, *cap); }
    std::memcpy(*buf + *pos, s, slen); *pos += slen;
}

static void json_stringify_internal(const McJson *j, char **buf, size_t *cap, size_t *pos) {
    if (!j) { append_str(buf, cap, pos, "null"); return; }
    switch (j->type) {
        case MC_JSON_NULL: append_str(buf, cap, pos, "null"); break;
        case MC_JSON_BOOL: append_str(buf, cap, pos, j->bool_value ? "true" : "false"); break;
        case MC_JSON_NUMBER: {
            std::ostringstream oss;
            if (j->number_value == (long long)j->number_value)
                oss << std::fixed << std::setprecision(0) << j->number_value;
            else
                oss << j->number_value;
            append_str(buf, cap, pos, oss.str().c_str());
            break;
        }
        case MC_JSON_STRING: {
            append_str(buf, cap, pos, "\"");
            for (const char *s = j->string_value; *s; s++) {
                switch (*s) {
                    case '"': append_str(buf, cap, pos, "\\\""); break;
                    case '\\': append_str(buf, cap, pos, "\\\\"); break;
                    case '\n': append_str(buf, cap, pos, "\\n"); break;
                    case '\r': append_str(buf, cap, pos, "\\r"); break;
                    case '\t': append_str(buf, cap, pos, "\\t"); break;
                    case '\b': append_str(buf, cap, pos, "\\b"); break;
                    case '\f': append_str(buf, cap, pos, "\\f"); break;
                    default: {
                        if ((unsigned char)*s < 0x20) {
                            std::ostringstream oss;
                            oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (unsigned)(unsigned char)*s;
                            append_str(buf, cap, pos, oss.str().c_str());
                        } else {
                            char esc[2] = { *s, '\0' };
                            append_str(buf, cap, pos, esc);
                        }
                        break;
                    }
                }
            }
            append_str(buf, cap, pos, "\"");
            break;
        }
        case MC_JSON_ARRAY: {
            append_str(buf, cap, pos, "[");
            const McJson *c = j->child;
            int first = 1;
            while (c) {
                if (!first) append_str(buf, cap, pos, ",");
                json_stringify_internal(c, buf, cap, pos);
                first = 0; c = c->next;
            }
            append_str(buf, cap, pos, "]");
            break;
        }
        case MC_JSON_OBJECT: {
            append_str(buf, cap, pos, "{");
            const McJson *c = j->child;
            int first = 1;
            while (c) {
                if (!first) append_str(buf, cap, pos, ",");
                if (c->key) {
                    append_str(buf, cap, pos, "\"");
                    append_str(buf, cap, pos, c->key);
                    append_str(buf, cap, pos, "\":");
                }
                json_stringify_internal(c, buf, cap, pos);
                first = 0; c = c->next;
            }
            append_str(buf, cap, pos, "}");
            break;
        }
    }
}

char *mc_json_stringify(const McJson *j) {
    size_t cap = 1024, pos = 0;
    char *buf = (char *)std::malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    json_stringify_internal(j, &buf, &cap, &pos);
    buf[pos] = '\0';
    return buf;
}

void mc_json_free(McJson *j) {
    if (!j) return;
    McJson *c = j->child;
    while (c) { McJson *n = c->next; mc_json_free(c); c = n; }
    std::free(j->string_value);
    std::free(j->key);
    std::free(j);
}

McJson *mc_json_get(const McJson *j, const char *key) {
    if (!j || !key || j->type != MC_JSON_OBJECT) return NULL;
    const McJson *c = j->child;
    while (c) {
        if (c->key && std::strcmp(c->key, key) == 0) return (McJson *)c;
        c = c->next;
    }
    return NULL;
}

const char *mc_json_get_string(const McJson *j, const char *key, const char *def) {
    McJson *v = mc_json_get(j, key);
    if (!v || v->type != MC_JSON_STRING) return def;
    return v->string_value ? v->string_value : def;
}

double mc_json_get_number(const McJson *j, const char *key, double def) {
    McJson *v = mc_json_get(j, key);
    if (!v || v->type != MC_JSON_NUMBER) return def;
    return v->number_value;
}

int mc_json_get_int(const McJson *j, const char *key, int def) {
    return (int)mc_json_get_number(j, key, (double)def);
}

int mc_json_get_bool(const McJson *j, const char *key, int def) {
    McJson *v = mc_json_get(j, key);
    if (!v || v->type != MC_JSON_BOOL) return def;
    return v->bool_value;
}

McJson *mc_json_get_array_item(const McJson *arr, int index) {
    if (!arr || arr->type != MC_JSON_ARRAY) return NULL;
    const McJson *c = arr->child;
    for (int i = 0; c && i < index; i++) c = c->next;
    return (McJson *)c;
}

int mc_json_array_length(const McJson *arr) {
    if (!arr || arr->type != MC_JSON_ARRAY) return 0;
    int count = 0;
    for (const McJson *c = arr->child; c; c = c->next) count++;
    return count;
}

int mc_json_object_foreach(const McJson *obj, int index, const char **key, McJson **val) {
    if (!obj || obj->type != MC_JSON_OBJECT) return 0;
    const McJson *c = obj->child;
    for (int i = 0; c && i < index; i++) c = c->next;
    if (!c) return 0;
    *key = c->key;
    *val = (McJson *)c;
    return 1;
}

McJson *mc_json_create_object(void) { return create_node(MC_JSON_OBJECT); }
McJson *mc_json_create_array(void) { return create_node(MC_JSON_ARRAY); }
McJson *mc_json_create_string(const char *s) { McJson *j = create_node(MC_JSON_STRING); if (j && s) j->string_value = mc_strdup(s); return j; }
McJson *mc_json_create_number(double n) { McJson *j = create_node(MC_JSON_NUMBER); if (j) j->number_value = n; return j; }
McJson *mc_json_create_bool(int b) { McJson *j = create_node(MC_JSON_BOOL); if (j) j->bool_value = b; return j; }
McJson *mc_json_create_null(void) { return create_node(MC_JSON_NULL); }

void mc_json_add_item(McJson *parent, McJson *item) {
    if (!parent || !item) return;
    if (!parent->child) parent->child = item;
    else { McJson *t = parent->child; while (t->next) t = t->next; t->next = item; item->prev = t; }
}

static McJson *find_key(McJson *parent, const char *key) {
    if (!parent || !key) return NULL;
    McJson *c = parent->child;
    while (c) { if (c->key && std::strcmp(c->key, key) == 0) return c; c = c->next; }
    return NULL;
}

void mc_json_add_string(McJson *parent, const char *key, const char *val) {
    McJson *existing = find_key(parent, key);
    if (existing) { std::free(existing->string_value); existing->type = MC_JSON_STRING; existing->string_value = mc_strdup(val); return; }
    McJson *item = mc_json_create_string(val);
    item->key = mc_strdup(key);
    mc_json_add_item(parent, item);
}

void mc_json_add_number(McJson *parent, const char *key, double val) {
    McJson *existing = find_key(parent, key);
    if (existing) { existing->type = MC_JSON_NUMBER; existing->number_value = val; return; }
    McJson *item = mc_json_create_number(val);
    item->key = mc_strdup(key);
    mc_json_add_item(parent, item);
}

void mc_json_add_bool(McJson *parent, const char *key, int val) {
    McJson *existing = find_key(parent, key);
    if (existing) { existing->type = MC_JSON_BOOL; existing->bool_value = val; return; }
    McJson *item = mc_json_create_bool(val);
    item->key = mc_strdup(key);
    mc_json_add_item(parent, item);
}

char *mc_json_get_string_ptr(const McJson *j) { return j && j->type == MC_JSON_STRING ? j->string_value : NULL; }
double mc_json_get_number_value(const McJson *j) { return j && j->type == MC_JSON_NUMBER ? j->number_value : 0.0; }
