#ifndef MC_JSON_H
#define MC_JSON_H

#include <stddef.h>

typedef enum { MC_JSON_NULL, MC_JSON_BOOL, MC_JSON_NUMBER, MC_JSON_STRING, MC_JSON_ARRAY, MC_JSON_OBJECT } McJsonType;

typedef struct McJson {
    McJsonType type;
    char *string_value;
    double number_value;
    int bool_value;
    struct McJson *child;
    struct McJson *next;
    struct McJson *prev;
    char *key;
} McJson;

McJson *mc_json_parse(const char *str);
McJson *mc_json_parse_file(const char *path);
char *mc_json_stringify(const McJson *j);

void mc_json_free(McJson *j);

McJson *mc_json_get(const McJson *j, const char *key);
const char *mc_json_get_string(const McJson *j, const char *key, const char *def);
double mc_json_get_number(const McJson *j, const char *key, double def);
int mc_json_get_int(const McJson *j, const char *key, int def);
int mc_json_get_bool(const McJson *j, const char *key, int def);
McJson *mc_json_get_array_item(const McJson *arr, int index);
int mc_json_array_length(const McJson *arr);
int mc_json_object_foreach(const McJson *obj, int index, const char **key, McJson **val);

McJson *mc_json_create_object(void);
McJson *mc_json_create_array(void);
McJson *mc_json_create_string(const char *s);
McJson *mc_json_create_number(double n);
McJson *mc_json_create_bool(int b);
McJson *mc_json_create_null(void);
void mc_json_add_item(McJson *parent, McJson *item);
void mc_json_add_string(McJson *parent, const char *key, const char *val);
void mc_json_add_number(McJson *parent, const char *key, double val);
void mc_json_add_bool(McJson *parent, const char *key, int val);

char *mc_json_get_string_ptr(const McJson *j);
double mc_json_get_number_value(const McJson *j);

#endif
