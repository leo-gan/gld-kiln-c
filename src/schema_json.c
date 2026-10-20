#include "kiln.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static kiln_err conv(kiln_arena *arena, const kiln_json *root, const kiln_json *node, kiln_schema **out, int depth);

static int name_required(const kiln_json *obj, const char *name) {
    const kiln_json *req = kiln_json_get(obj, "required");
    size_t i;
    if (!req) {
        return 1;
    }
    if (req->kind != KILN_JSON_ARR) {
        return 0;
    }
    for (i = 0; i < req->n; i++) {
        if (req->vals[i].kind == KILN_JSON_STR && strcmp(req->vals[i].str, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static kiln_kind int_kind(const kiln_json *node) {
    const kiln_json *fmt = kiln_json_get(node, "format");
    if (!fmt) {
        return KILN_I64;
    }
    if (fmt->kind != KILN_JSON_STR) {
        return 0;
    }
    if (strcmp(fmt->str, "int8") == 0) return KILN_I8;
    if (strcmp(fmt->str, "int16") == 0) return KILN_I16;
    if (strcmp(fmt->str, "int32") == 0) return KILN_I32;
    if (strcmp(fmt->str, "int64") == 0) return KILN_I64;
    if (strcmp(fmt->str, "uint8") == 0) return KILN_U8;
    if (strcmp(fmt->str, "uint16") == 0) return KILN_U16;
    if (strcmp(fmt->str, "uint32") == 0) return KILN_U32;
    if (strcmp(fmt->str, "uint64") == 0) return KILN_U64;
    return 0;
}

static kiln_kind num_kind(const kiln_json *node) {
    const kiln_json *fmt = kiln_json_get(node, "format");
    if (!fmt) {
        return KILN_F64;
    }
    if (fmt->kind != KILN_JSON_STR) {
        return 0;
    }
    if (strcmp(fmt->str, "float") == 0) return KILN_F32;
    if (strcmp(fmt->str, "double") == 0) return KILN_F64;
    return 0;
}

static kiln_err wrap_opt(kiln_arena *arena, kiln_schema *inner, kiln_schema **out) {
    kiln_schema *opt = kiln_schema_new(arena, KILN_OPT);
    if (!opt) {
        return KILN_ERR_LIMIT;
    }
    opt->elem = inner;
    *out = opt;
    return KILN_OK;
}

static kiln_err conv(kiln_arena *arena, const kiln_json *root, const kiln_json *node, kiln_schema **out, int depth) {
    const kiln_json *ref;
    const kiln_json *type;
    kiln_kind kind;
    if (!node || node->kind != KILN_JSON_OBJ || depth > 64) {
        return KILN_ERR_SCHEMA;
    }
    ref = kiln_json_get(node, "$ref");
    if (ref) {
        const kiln_json *defs;
        const kiln_json *target;
        const char *prefix = "#/$defs/";
        size_t plen = strlen(prefix);
        if (ref->kind != KILN_JSON_STR || strncmp(ref->str, prefix, plen) != 0) {
            return KILN_ERR_SCHEMA;
        }
        defs = kiln_json_get(root, "$defs");
        target = defs ? kiln_json_get(defs, ref->str + plen) : NULL;
        if (!target) {
            return KILN_ERR_SCHEMA;
        }
        return conv(arena, root, target, out, depth + 1);
    }
    type = kiln_json_get(node, "type");
    if (!type || type->kind != KILN_JSON_STR) {
        return KILN_ERR_SCHEMA;
    }
    if (strcmp(type->str, "boolean") == 0) {
        kind = KILN_BOOL;
    } else if (strcmp(type->str, "integer") == 0) {
        kind = int_kind(node);
    } else if (strcmp(type->str, "number") == 0) {
        kind = num_kind(node);
    } else if (strcmp(type->str, "string") == 0) {
        const kiln_json *fmt = kiln_json_get(node, "format");
        kind = KILN_STR;
        if (fmt) {
            if (fmt->kind != KILN_JSON_STR || strcmp(fmt->str, "binary") != 0) {
                return KILN_ERR_SCHEMA;
            }
            kind = KILN_BYTES;
        }
    } else if (strcmp(type->str, "array") == 0) {
        const kiln_json *items = kiln_json_get(node, "items");
        kiln_schema *elem = NULL;
        kiln_schema *list;
        kiln_err err = conv(arena, root, items, &elem, depth + 1);
        if (err) {
            return err;
        }
        list = kiln_schema_new(arena, KILN_LIST);
        if (!list) {
            return KILN_ERR_LIMIT;
        }
        list->elem = elem;
        *out = list;
        return KILN_OK;
    } else if (strcmp(type->str, "object") == 0) {
        const kiln_json *props = kiln_json_get(node, "properties");
        const kiln_json *extra = kiln_json_get(node, "additionalProperties");
        if (props && props->kind == KILN_JSON_OBJ) {
            kiln_field *fields;
            kiln_schema *st;
            size_t i;
            if (props->n) {
                fields = kiln_arena_alloc(arena, props->n * sizeof(*fields), sizeof(void *));
                if (!fields) {
                    return KILN_ERR_LIMIT;
                }
            } else {
                fields = NULL;
            }
            for (i = 0; i < props->n; i++) {
                kiln_schema *inner = NULL;
                kiln_err err = conv(arena, root, &props->vals[i], &inner, depth + 1);
                int req;
                if (err) {
                    return err;
                }
                fields[i].name = kiln_arena_str(arena, props->keys[i], strlen(props->keys[i]));
                if (!fields[i].name) {
                    return KILN_ERR_LIMIT;
                }
                req = name_required(node, props->keys[i]);
                if (!req) {
                    err = wrap_opt(arena, inner, &inner);
                    if (err) {
                        return err;
                    }
                }
                fields[i].type = inner;
            }
            st = kiln_schema_new(arena, KILN_STRUCT);
            if (!st) {
                return KILN_ERR_LIMIT;
            }
            st->fields = fields;
            st->nfields = props->n;
            *out = st;
            return KILN_OK;
        }
        if (extra && extra->kind == KILN_JSON_OBJ) {
            kiln_schema *elem = NULL;
            kiln_schema *dict;
            kiln_err err = conv(arena, root, extra, &elem, depth + 1);
            if (err) {
                return err;
            }
            dict = kiln_schema_new(arena, KILN_DICT);
            if (!dict) {
                return KILN_ERR_LIMIT;
            }
            dict->elem = elem;
            *out = dict;
            return KILN_OK;
        }
        return KILN_ERR_SCHEMA;
    } else {
        return KILN_ERR_SCHEMA;
    }
    if (!kind) {
        return KILN_ERR_SCHEMA;
    }
    *out = kiln_schema_new(arena, kind);
    if (!*out) {
        return KILN_ERR_LIMIT;
    }
    return KILN_OK;
}

kiln_err kiln_schema_from_json(kiln_arena *arena, const char *json, size_t len, kiln_schema **out) {
    const char *err = NULL;
    kiln_json *root = kiln_json_parse(json, len, &err);
    kiln_err rc;
    if (!root) {
        return KILN_ERR_SCHEMA;
    }
    rc = conv(arena, root, root, out, 0);
    kiln_json_free(root);
    return rc;
}
