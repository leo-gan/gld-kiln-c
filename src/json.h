#ifndef KILN_JSON_H
#define KILN_JSON_H

#include <stddef.h>

typedef struct kiln_json kiln_json;

enum {
    KILN_JSON_NULL = 0,
    KILN_JSON_BOOL = 1,
    KILN_JSON_NUM = 2,
    KILN_JSON_STR = 3,
    KILN_JSON_ARR = 4,
    KILN_JSON_OBJ = 5
};

struct kiln_json {
    int kind;
    int b;
    char *str;
    size_t n;
    char **keys;
    kiln_json *vals;
};

kiln_json *kiln_json_parse(const char *text, size_t len, const char **err);
void kiln_json_free(kiln_json *value);
const kiln_json *kiln_json_get(const kiln_json *obj, const char *key);

#endif
