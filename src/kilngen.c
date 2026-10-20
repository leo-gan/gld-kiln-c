#include "json.h"
#include "kiln.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GK_BOOL,
    GK_I8,
    GK_I16,
    GK_I32,
    GK_I64,
    GK_U8,
    GK_U16,
    GK_U32,
    GK_U64,
    GK_F32,
    GK_F64,
    GK_STR,
    GK_BYTES,
    GK_STRUCT,
    GK_LIST,
    GK_DICT
};

typedef struct GType GType;
typedef struct GField GField;

struct GField {
    char *name;
    GType *type;
    int required;
};

struct GType {
    int kind;
    char *name;
    GField *fields;
    size_t nfields;
    GType *elem;
    int emitted;
    int visiting;
};

static GType *gstructs[64];
static size_t ngstructs;
static int failed;
static const char *cur_struct;

static void die(const char *msg) {
    fprintf(stderr, "gld-kilngen-c: %s\n", msg);
    exit(1);
}

static char *xdup(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (!d) {
        die("out of memory");
    }
    memcpy(d, s, n + 1);
    return d;
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) {
        die("cannot open schema");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        die("seek");
    }
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n + 1);
    if (!buf) {
        die("out of memory");
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        die("read");
    }
    buf[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int ident_ok(const char *s) {
    size_t i;
    if (!s || !(isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return 0;
    }
    for (i = 1; s[i]; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return 0;
        }
    }
    return 1;
}

static GType *scalar(int kind) {
    static GType table[12];
    static int ready;
    if (!ready) {
        int i;
        for (i = 0; i < 12; i++) {
            table[i].kind = i;
        }
        ready = 1;
    }
    return &table[kind];
}

static GType *find_struct(const char *name) {
    size_t i;
    for (i = 0; i < ngstructs; i++) {
        if (strcmp(gstructs[i]->name, name) == 0) {
            return gstructs[i];
        }
    }
    return NULL;
}

static GType *new_struct(const char *name) {
    GType *t = find_struct(name);
    if (t) {
        return t;
    }
    if (ngstructs >= 64) {
        die("too many structs");
    }
    if (!ident_ok(name)) {
        die("struct name is not a C identifier");
    }
    t = calloc(1, sizeof(*t));
    if (!t) {
        die("out of memory");
    }
    t->kind = GK_STRUCT;
    t->name = xdup(name);
    gstructs[ngstructs++] = t;
    return t;
}

static int prop_required(const kiln_json *obj, const char *name) {
    const kiln_json *req = kiln_json_get(obj, "required");
    size_t i;
    if (!req) {
        return 1;
    }
    if (req->kind != KILN_JSON_ARR) {
        die("required must be an array");
    }
    for (i = 0; i < req->n; i++) {
        if (req->vals[i].kind == KILN_JSON_STR && strcmp(req->vals[i].str, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static GType *convert(const kiln_json *root, const kiln_json *node, const char *sname);

static void fill_struct(GType *st, const kiln_json *root, const kiln_json *node) {
    const kiln_json *props = kiln_json_get(node, "properties");
    size_t i;
    if (st->fields || st->nfields) {
        return;
    }
    if (!props || props->kind != KILN_JSON_OBJ) {
        die("struct is missing properties");
    }
    st->nfields = props->n;
    if (props->n) {
        st->fields = calloc(props->n, sizeof(GField));
        if (!st->fields) {
            die("out of memory");
        }
    }
    for (i = 0; i < props->n; i++) {
        char child[256];
        if (!ident_ok(props->keys[i])) {
            die("property name is not a C identifier");
        }
        snprintf(child, sizeof child, "%s_%s", st->name, props->keys[i]);
        st->fields[i].name = xdup(props->keys[i]);
        st->fields[i].required = prop_required(node, props->keys[i]);
        st->fields[i].type = convert(root, &props->vals[i], child);
        if (!st->fields[i].type) {
            die("bad property schema");
        }
    }
}

static int kind_from_format_int(const char *fmt) {
    if (!fmt) return GK_I64;
    if (strcmp(fmt, "int8") == 0) return GK_I8;
    if (strcmp(fmt, "int16") == 0) return GK_I16;
    if (strcmp(fmt, "int32") == 0) return GK_I32;
    if (strcmp(fmt, "int64") == 0) return GK_I64;
    if (strcmp(fmt, "uint8") == 0) return GK_U8;
    if (strcmp(fmt, "uint16") == 0) return GK_U16;
    if (strcmp(fmt, "uint32") == 0) return GK_U32;
    if (strcmp(fmt, "uint64") == 0) return GK_U64;
    return -1;
}

static GType *convert(const kiln_json *root, const kiln_json *node, const char *sname) {
    const kiln_json *ref;
    const kiln_json *type;
    const kiln_json *fmt;
    if (!node || node->kind != KILN_JSON_OBJ) {
        die("schema node");
    }
    ref = kiln_json_get(node, "$ref");
    if (ref) {
        const char *prefix = "#/$defs/";
        GType *st;
        if (ref->kind != KILN_JSON_STR || strncmp(ref->str, prefix, strlen(prefix)) != 0) {
            die("only #/$defs/Name refs are supported");
        }
        st = find_struct(ref->str + strlen(prefix));
        if (!st) {
            die("unknown $ref");
        }
        return st;
    }
    type = kiln_json_get(node, "type");
    if (!type || type->kind != KILN_JSON_STR) {
        die("missing type");
    }
    fmt = kiln_json_get(node, "format");
    if (strcmp(type->str, "boolean") == 0) return scalar(GK_BOOL);
    if (strcmp(type->str, "integer") == 0) {
        int k = kind_from_format_int(fmt && fmt->kind == KILN_JSON_STR ? fmt->str : NULL);
        if (k < 0) die("bad integer format");
        return scalar(k);
    }
    if (strcmp(type->str, "number") == 0) {
        if (!fmt) return scalar(GK_F64);
        if (fmt->kind == KILN_JSON_STR && strcmp(fmt->str, "float") == 0) return scalar(GK_F32);
        if (fmt->kind == KILN_JSON_STR && strcmp(fmt->str, "double") == 0) return scalar(GK_F64);
        die("bad number format");
    }
    if (strcmp(type->str, "string") == 0) {
        if (!fmt) return scalar(GK_STR);
        if (fmt->kind == KILN_JSON_STR && strcmp(fmt->str, "binary") == 0) return scalar(GK_BYTES);
        die("bad string format");
    }
    if (strcmp(type->str, "array") == 0) {
        GType *list = calloc(1, sizeof(*list));
        const kiln_json *items = kiln_json_get(node, "items");
        if (!list) die("out of memory");
        list->kind = GK_LIST;
        list->elem = convert(root, items, sname);
        return list;
    }
    if (strcmp(type->str, "object") == 0) {
        const kiln_json *props = kiln_json_get(node, "properties");
        const kiln_json *extra = kiln_json_get(node, "additionalProperties");
        if (props) {
            GType *st = new_struct(sname);
            fill_struct(st, root, node);
            return st;
        }
        if (extra && extra->kind == KILN_JSON_OBJ) {
            GType *dict = calloc(1, sizeof(*dict));
            if (!dict) die("out of memory");
            dict->kind = GK_DICT;
            dict->elem = convert(root, extra, sname);
            return dict;
        }
        die("object needs properties or additionalProperties");
    }
    die("unsupported JSON Schema type");
    return NULL;
}

static const char *scalar_ctype(int kind) {
    switch (kind) {
    case GK_BOOL: return "int";
    case GK_I8: return "int8_t";
    case GK_I16: return "int16_t";
    case GK_I32: return "int32_t";
    case GK_I64: return "int64_t";
    case GK_U8: return "uint8_t";
    case GK_U16: return "uint16_t";
    case GK_U32: return "uint32_t";
    case GK_U64: return "uint64_t";
    case GK_F32: return "float";
    case GK_F64: return "double";
    default: return NULL;
    }
}

static int scalar_width(int kind) {
    switch (kind) {
    case GK_BOOL:
    case GK_I8:
    case GK_U8:
        return 1;
    case GK_I16:
    case GK_U16:
        return 2;
    case GK_I32:
    case GK_U32:
    case GK_F32:
        return 4;
    case GK_I64:
    case GK_U64:
    case GK_F64:
        return 8;
    default:
        return 0;
    }
}

static size_t type_min(const GType *t) {
    size_t i;
    size_t n = 0;
    int w;
    switch (t->kind) {
    case GK_STR:
    case GK_BYTES:
    case GK_LIST:
    case GK_DICT:
        return 4;
    case GK_STRUCT:
        for (i = 0; i < t->nfields; i++) {
            if (!t->fields[i].required) {
                n += 1;
            } else {
                n += type_min(t->fields[i].type);
            }
        }
        return n;
    default:
        w = scalar_width(t->kind);
        return w ? (size_t)w : 0;
    }
}

static const char *store_fn(int kind) {
    switch (kind) {
    case GK_I8:
    case GK_U8:
    case GK_BOOL:
        return NULL;
    case GK_I16:
    case GK_U16:
        return "kiln_store_u16";
    case GK_I32:
        return "kiln_store_i32";
    case GK_U32:
        return "kiln_store_u32";
    case GK_I64:
        return "kiln_store_i64";
    case GK_U64:
        return "kiln_store_u64";
    case GK_F32:
        return "kiln_store_f32";
    case GK_F64:
        return "kiln_store_f64";
    default:
        return NULL;
    }
}

static const char *load_expr(int kind) {
    switch (kind) {
    case GK_I8: return "kiln_load_i8";
    case GK_I16: return "kiln_load_i16";
    case GK_I32: return "kiln_load_i32";
    case GK_I64: return "kiln_load_i64";
    case GK_U16: return "kiln_load_u16";
    case GK_U32: return "kiln_load_u32";
    case GK_U64: return "kiln_load_u64";
    case GK_F32: return "kiln_load_f32";
    case GK_F64: return "kiln_load_f64";
    default: return NULL;
    }
}

static void emit_store(FILE *f, int kind, const char *dst, const char *val) {
    const char *fn = store_fn(kind);
    if (kind == GK_BOOL) {
        fprintf(f, "%s = (uint8_t)(%s ? 1 : 0);", dst, val);
    } else if (kind == GK_I8 || kind == GK_U8) {
        fprintf(f, "%s = (uint8_t)%s;", dst, val);
    } else if (kind == GK_I16) {
        fprintf(f, "kiln_store_u16(%s, (uint16_t)%s);", dst, val);
    } else if (fn) {
        fprintf(f, "%s(%s, %s);", fn, dst, val);
    }
}

static int list_items_are(const GType *t, int kind) {
    return t->kind == GK_LIST && t->elem && t->elem->kind == kind;
}

static void emit_decls(FILE *f, const GField *field) {
    const GType *t = field->type;
    const char *ct;
    if (!field->required) {
        fprintf(f, "    int %s_present;\n", field->name);
    }
    if (t->kind == GK_STRUCT) {
        fprintf(f, "    %s %s;\n", t->name, field->name);
        return;
    }
    if (t->kind == GK_STR) {
        fprintf(f, "    char *%s;\n", field->name);
        fprintf(f, "    size_t %s_len;\n", field->name);
        return;
    }
    if (t->kind == GK_BYTES) {
        fprintf(f, "    uint8_t *%s;\n", field->name);
        fprintf(f, "    size_t %s_len;\n", field->name);
        return;
    }
    if (t->kind == GK_LIST) {
        if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "    %s *%s;\n", t->elem->name, field->name);
        } else if (t->elem->kind == GK_STR) {
            fprintf(f, "    %s_%s_item *%s;\n", "ITEM", field->name, field->name);
        } else if (t->elem->kind == GK_BYTES) {
            fprintf(f, "    %s_%s_item *%s;\n", "ITEM", field->name, field->name);
        } else if ((ct = scalar_ctype(t->elem->kind)) != NULL) {
            fprintf(f, "    %s *%s;\n", ct, field->name);
        } else {
            die("unsupported list element");
        }
        fprintf(f, "    size_t %s_count;\n", field->name);
        return;
    }
    ct = scalar_ctype(t->kind);
    if (!ct) {
        die("unsupported field");
    }
    fprintf(f, "    %s %s;\n", ct, field->name);
}

static void emit_item_typedefs(FILE *f, const GType *st) {
    size_t i;
    for (i = 0; i < st->nfields; i++) {
        const GType *t = st->fields[i].type;
        if (t->kind == GK_LIST && t->elem && (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES)) {
            const char *ptr = t->elem->kind == GK_STR ? "char" : "uint8_t";
            fprintf(f, "typedef struct %s_%s_item {\n", st->name, st->fields[i].name);
            fprintf(f, "    %s *data;\n", ptr);
            fprintf(f, "    size_t len;\n");
            fprintf(f, "} %s_%s_item;\n\n", st->name, st->fields[i].name);
        }
    }
}

static void emit_header(FILE *f, const char *stem) {
    size_t i;
    char guard[256];
    snprintf(guard, sizeof guard, "KILN_GEN_%s_H", stem);
    for (i = 0; guard[i]; i++) {
        guard[i] = (char)toupper((unsigned char)guard[i]);
    }
    fprintf(f, "/* Generated by gld-kilngen-c. Do not edit. */\n");
    fprintf(f, "#ifndef %s\n#define %s\n\n", guard, guard);
    fprintf(f, "#include \"kiln.h\"\n\n");
    fprintf(f, "#include <stddef.h>\n#include <stdint.h>\n\n");
    for (i = 0; i < ngstructs; i++) {
        fprintf(f, "typedef struct %s %s;\n", gstructs[i]->name, gstructs[i]->name);
    }
    fprintf(f, "\n");
    for (i = 0; i < ngstructs; i++) {
        size_t fdi;
        const GType *st = gstructs[i];
        emit_item_typedefs(f, st);
        fprintf(f, "struct %s {\n", st->name);
        for (fdi = 0; fdi < st->nfields; fdi++) {
            /* Item typedef names use the parent struct, so fix the placeholder. */
            emit_decls(f, &st->fields[fdi]);
        }
        fprintf(f, "};\n\n");
        fprintf(f, "size_t %s_encoded_size(const %s *v);\n", st->name, st->name);
        fprintf(f, "kiln_err %s_encode(const %s *v, uint8_t *dst, size_t cap, size_t *written);\n", st->name, st->name);
        fprintf(f, "kiln_err %s_decode_into(struct %s *out, const uint8_t *src, size_t len, size_t *used);\n", st->name, st->name);
        fprintf(f, "kiln_err %s_decode(struct %s *out, const uint8_t *src, size_t len);\n", st->name, st->name);
        fprintf(f, "void %s_free(struct %s *v);\n\n", st->name, st->name);
    }
    fprintf(f, "#endif\n");
}

/* emit_decls for list-of-string used a placeholder ITEM. Rewrite that path. */
static void emit_decls_fixed(FILE *f, const char *parent, const GField *field) {
    const GType *t = field->type;
    const char *ct;
    if (!field->required) {
        fprintf(f, "    int %s_present;\n", field->name);
    }
    if (t->kind == GK_STRUCT) {
        fprintf(f, "    %s %s;\n", t->name, field->name);
        return;
    }
    if (t->kind == GK_STR) {
        fprintf(f, "    char *%s;\n    size_t %s_len;\n", field->name, field->name);
        return;
    }
    if (t->kind == GK_BYTES) {
        fprintf(f, "    uint8_t *%s;\n    size_t %s_len;\n", field->name, field->name);
        return;
    }
    if (t->kind == GK_LIST) {
        if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "    %s *%s;\n", t->elem->name, field->name);
        } else if (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES) {
            fprintf(f, "    %s_%s_item *%s;\n", parent, field->name, field->name);
        } else if ((ct = scalar_ctype(t->elem->kind)) != NULL) {
            fprintf(f, "    %s *%s;\n", ct, field->name);
        } else {
            die("unsupported list element");
        }
        fprintf(f, "    size_t %s_count;\n", field->name);
        return;
    }
    if (t->kind == GK_DICT) {
        die("dict fields are not emitted as bare scalars; use additionalProperties at the root");
    }
    ct = scalar_ctype(t->kind);
    if (!ct) die("unsupported field");
    fprintf(f, "    %s %s;\n", ct, field->name);
    (void)list_items_are;
}

static void emit_struct_body(FILE *f, GType *st) {
    size_t fi;
    if (st->emitted) {
        return;
    }
    if (st->visiting) {
        die("struct cycle");
    }
    st->visiting = 1;
    for (fi = 0; fi < st->nfields; fi++) {
        GType *t = st->fields[fi].type;
        if (t->kind == GK_STRUCT) {
            emit_struct_body(f, t);
        }
    }
    st->visiting = 0;
    st->emitted = 1;
    emit_item_typedefs(f, st);
    fprintf(f, "struct %s {\n", st->name);
    for (fi = 0; fi < st->nfields; fi++) {
        emit_decls_fixed(f, st->name, &st->fields[fi]);
    }
    fprintf(f, "};\n\n");
}

static void emit_header2(FILE *f, const char *stem) {
    size_t i;
    char guard[256];
    snprintf(guard, sizeof guard, "KILN_GEN_%s_H", stem);
    for (i = 0; guard[i]; i++) {
        if (guard[i] == '.') {
            guard[i] = '_';
        }
        guard[i] = (char)toupper((unsigned char)guard[i]);
    }
    fprintf(f, "/* Generated by gld-kilngen-c. Do not edit. */\n");
    fprintf(f, "#ifndef %s\n#define %s\n\n", guard, guard);
    fprintf(f, "#include \"kiln.h\"\n\n");
    for (i = 0; i < ngstructs; i++) {
        fprintf(f, "typedef struct %s %s;\n", gstructs[i]->name, gstructs[i]->name);
    }
    if (ngstructs) {
        fprintf(f, "\n");
    }
    for (i = 0; i < ngstructs; i++) {
        emit_struct_body(f, gstructs[i]);
    }
    for (i = 0; i < ngstructs; i++) {
        fprintf(f, "size_t %s_encoded_size(const struct %s *v);\n", gstructs[i]->name, gstructs[i]->name);
        fprintf(f, "kiln_err %s_encode(const struct %s *v, uint8_t *dst, size_t cap, size_t *written);\n",
            gstructs[i]->name, gstructs[i]->name);
        fprintf(f, "kiln_err %s_decode_into(struct %s *out, const uint8_t *src, size_t len, size_t *used);\n",
            gstructs[i]->name, gstructs[i]->name);
        fprintf(f, "kiln_err %s_decode(struct %s *out, const uint8_t *src, size_t len);\n", gstructs[i]->name,
            gstructs[i]->name);
        fprintf(f, "void %s_free(struct %s *v);\n\n", gstructs[i]->name, gstructs[i]->name);
    }
    fprintf(f, "#endif\n");
    (void)emit_decls;
    (void)emit_header;
}

static void emit_free_field(FILE *f, const char *parent, const GField *field) {
    const GType *t = field->type;
    if (t->kind == GK_STR || t->kind == GK_BYTES) {
        fprintf(f, "    free(v->%s);\n    v->%s = NULL;\n    v->%s_len = 0;\n", field->name, field->name, field->name);
    } else if (t->kind == GK_STRUCT) {
        fprintf(f, "    %s_free(&v->%s);\n", t->name, field->name);
    } else if (t->kind == GK_LIST) {
        if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "    if (v->%s) {\n        size_t i;\n        for (i = 0; i < v->%s_count; i++) %s_free(&v->%s[i]);\n    }\n",
                field->name, field->name, t->elem->name, field->name);
        } else if (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES) {
            fprintf(f, "    if (v->%s) {\n        size_t i;\n        for (i = 0; i < v->%s_count; i++) free(v->%s[i].data);\n    }\n",
                field->name, field->name, field->name);
        }
        fprintf(f, "    free(v->%s);\n    v->%s = NULL;\n    v->%s_count = 0;\n", field->name, field->name, field->name);
    }
    if (!field->required) {
        fprintf(f, "    v->%s_present = 0;\n", field->name);
    }
    (void)parent;
}

static void emit_size_field(FILE *f, const GField *field) {
    const GType *t = field->type;
    int w = scalar_width(t->kind);
    if (!field->required) {
        fprintf(f, "    n += 1;\n    if (v->%s_present) {\n", field->name);
    }
    if (w) {
        fprintf(f, "    n += %d;\n", w);
    } else if (t->kind == GK_STR || t->kind == GK_BYTES) {
        fprintf(f, "    n += 4 + v->%s_len;\n", field->name);
    } else if (t->kind == GK_STRUCT) {
        fprintf(f, "    n += %s_encoded_size(&v->%s);\n", t->name, field->name);
    } else if (t->kind == GK_LIST) {
        int ew = scalar_width(t->elem->kind);
        fprintf(f, "    n += 4;\n");
        if (ew) {
            fprintf(f, "    n += v->%s_count * %d;\n", field->name, ew);
        } else if (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES) {
            fprintf(f, "    {\n        size_t i;\n        for (i = 0; i < v->%s_count; i++) n += 4 + v->%s[i].len;\n    }\n",
                field->name, field->name);
        } else if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "    {\n        size_t i;\n        for (i = 0; i < v->%s_count; i++) n += %s_encoded_size(&v->%s[i]);\n    }\n",
                field->name, t->elem->name, field->name);
        }
    }
    if (!field->required) {
        fprintf(f, "    }\n");
    }
}

static void emit_encode_scalar(FILE *f, int kind, const char *access) {
    int w = scalar_width(kind);
    fprintf(f, "        ");
    if (kind == GK_BOOL || kind == GK_I8 || kind == GK_U8) {
        emit_store(f, kind, "dst[pos]", access);
        fprintf(f, "\n        pos += 1;\n");
    } else {
        emit_store(f, kind, "dst + pos", access);
        fprintf(f, "\n        pos += %d;\n", w);
    }
}

static void emit_encode_field(FILE *f, const GField *field) {
    const GType *t = field->type;
    if (!field->required) {
        fprintf(f, "    if (!v->%s_present) {\n        dst[pos++] = 0;\n    } else {\n        dst[pos++] = 1;\n", field->name);
    }
    if (scalar_width(t->kind)) {
        char access[128];
        snprintf(access, sizeof access, "v->%s", field->name);
        emit_encode_scalar(f, t->kind, access);
    } else if (t->kind == GK_STR || t->kind == GK_BYTES) {
        fprintf(f, "        if (v->%s_len > 0xffffffffu) return KILN_ERR_LIMIT;\n", field->name);
        fprintf(f, "        kiln_store_u32(dst + pos, (uint32_t)v->%s_len);\n        pos += 4;\n", field->name);
        fprintf(f, "        if (v->%s_len) memcpy(dst + pos, v->%s, v->%s_len);\n        pos += v->%s_len;\n",
            field->name, field->name, field->name, field->name);
    } else if (t->kind == GK_STRUCT) {
        fprintf(f, "        {\n            size_t child = 0;\n            kiln_err err = %s_encode(&v->%s, dst + pos, cap - pos, &child);\n",
            t->name, field->name);
        fprintf(f, "            if (err) return err;\n            pos += child;\n        }\n");
    } else if (t->kind == GK_LIST) {
        int ew = scalar_width(t->elem->kind);
        fprintf(f, "        if (v->%s_count > 0xffffffffu) return KILN_ERR_LIMIT;\n", field->name);
        fprintf(f, "        kiln_store_u32(dst + pos, (uint32_t)v->%s_count);\n        pos += 4;\n", field->name);
        if (ew && (t->elem->kind == GK_I8 || t->elem->kind == GK_U8 || t->elem->kind == GK_BOOL || ew > 1)) {
            if (t->elem->kind == GK_BOOL || t->elem->kind == GK_I8 || t->elem->kind == GK_U8 ||
                t->elem->kind == GK_I16 || t->elem->kind == GK_U16 || t->elem->kind == GK_I32 ||
                t->elem->kind == GK_U32 || t->elem->kind == GK_I64 || t->elem->kind == GK_U64 ||
                t->elem->kind == GK_F32 || t->elem->kind == GK_F64) {
                fprintf(f, "        {\n            size_t i;\n            for (i = 0; i < v->%s_count; i++) {\n", field->name);
                if (t->elem->kind == GK_BOOL || t->elem->kind == GK_I8 || t->elem->kind == GK_U8) {
                    fprintf(f, "                ");
                    emit_store(f, t->elem->kind, "dst[pos]", t->elem->kind == GK_BOOL ? "v->" : "v->");
                }
                /* rewritten below per element inside the loop by a second fprintf */
                fprintf(f, "\n            }\n        }\n");
            }
        } else if (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES) {
            fprintf(f, "        {\n            size_t i;\n            for (i = 0; i < v->%s_count; i++) {\n", field->name);
            fprintf(f, "                kiln_store_u32(dst + pos, (uint32_t)v->%s[i].len);\n                pos += 4;\n", field->name);
            fprintf(f, "                if (v->%s[i].len) memcpy(dst + pos, v->%s[i].data, v->%s[i].len);\n                pos += v->%s[i].len;\n",
                field->name, field->name, field->name, field->name);
            fprintf(f, "            }\n        }\n");
        } else if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "        {\n            size_t i;\n            for (i = 0; i < v->%s_count; i++) {\n", field->name);
            fprintf(f, "                size_t child = 0;\n                kiln_err err = %s_encode(&v->%s[i], dst + pos, cap - pos, &child);\n",
                t->elem->name, field->name);
            fprintf(f, "                if (err) return err;\n                pos += child;\n            }\n        }\n");
        }
    }
    if (!field->required) {
        fprintf(f, "    }\n");
    }
}

/* The list-of-scalar encoder above is incomplete. Emit a clean version. */
static void emit_encode_field2(FILE *f, const GField *field) {
    const GType *t = field->type;
    char access[160];
    if (!field->required) {
        fprintf(f, "    if (!v->%s_present) {\n        dst[pos++] = 0;\n    } else {\n        dst[pos++] = 1;\n", field->name);
    }
    if (scalar_width(t->kind)) {
        snprintf(access, sizeof access, "v->%s", field->name);
        emit_encode_scalar(f, t->kind, access);
    } else if (t->kind == GK_STR || t->kind == GK_BYTES) {
        fprintf(f, "        if (v->%s_len > 0xffffffffu) return KILN_ERR_LIMIT;\n", field->name);
        fprintf(f, "        kiln_store_u32(dst + pos, (uint32_t)v->%s_len);\n        pos += 4;\n", field->name);
        fprintf(f, "        if (v->%s_len && v->%s) memcpy(dst + pos, v->%s, v->%s_len);\n        pos += v->%s_len;\n",
            field->name, field->name, field->name, field->name, field->name);
    } else if (t->kind == GK_STRUCT) {
        fprintf(f, "        {\n            size_t child = 0;\n            kiln_err err = %s_encode(&v->%s, dst + pos, cap - pos, &child);\n",
            t->name, field->name);
        fprintf(f, "            if (err) return err;\n            pos += child;\n        }\n");
    } else if (t->kind == GK_LIST) {
        int ew = scalar_width(t->elem->kind);
        fprintf(f, "        if (v->%s_count > 0xffffffffu) return KILN_ERR_LIMIT;\n", field->name);
        fprintf(f, "        kiln_store_u32(dst + pos, (uint32_t)v->%s_count);\n        pos += 4;\n", field->name);
        fprintf(f, "        {\n            size_t i;\n            for (i = 0; i < v->%s_count; i++) {\n", field->name);
        if (ew) {
            snprintf(access, sizeof access, "v->%s[i]", field->name);
            fprintf(f, "            ");
            if (t->elem->kind == GK_BOOL || t->elem->kind == GK_I8 || t->elem->kind == GK_U8) {
                emit_store(f, t->elem->kind, "dst[pos]", access);
                fprintf(f, "\n                pos += 1;\n");
            } else {
                emit_store(f, t->elem->kind, "dst + pos", access);
                fprintf(f, "\n                pos += %d;\n", ew);
            }
        } else if (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES) {
            fprintf(f, "                kiln_store_u32(dst + pos, (uint32_t)v->%s[i].len);\n                pos += 4;\n", field->name);
            fprintf(f, "                if (v->%s[i].len && v->%s[i].data) memcpy(dst + pos, v->%s[i].data, v->%s[i].len);\n",
                field->name, field->name, field->name, field->name);
            fprintf(f, "                pos += v->%s[i].len;\n", field->name);
        } else if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "                size_t child = 0;\n                kiln_err err = %s_encode(&v->%s[i], dst + pos, cap - pos, &child);\n",
                t->elem->name, field->name);
            fprintf(f, "                if (err) return err;\n                pos += child;\n");
        } else {
            die("cannot encode list element");
        }
        fprintf(f, "            }\n        }\n");
    } else {
        die("cannot encode field");
    }
    if (!field->required) {
        fprintf(f, "    }\n");
    }
}

static void emit_decode_scalar(FILE *f, int kind, const char *dest) {
    int w = scalar_width(kind);
    const char *load = load_expr(kind);
    fprintf(f, "        if (pos + %d > len) return %s_fail(out, KILN_ERR_EOF);\n", w, cur_struct);
    if (kind == GK_BOOL) {
        fprintf(f, "        if (src[pos] > 1) return %s_fail(out, KILN_ERR_TYPE);\n", cur_struct);
        fprintf(f, "        %s = src[pos];\n        pos += 1;\n", dest);
    } else if (kind == GK_U8) {
        fprintf(f, "        %s = src[pos];\n        pos += 1;\n", dest);
    } else if (load) {
        fprintf(f, "        %s = %s(src + pos);\n        pos += %d;\n", dest, load, w);
    }
}

static void emit_read_text(FILE *f, const char *ptr, const char *len_field, int binary) {
    fprintf(f, "        {\n            uint32_t n;\n");
    fprintf(f, "            if (pos + 4 > len) return %s_fail(out, KILN_ERR_EOF);\n", cur_struct);
    fprintf(f, "            n = kiln_load_u32(src + pos);\n            pos += 4;\n");
    fprintf(f, "            if ((uint64_t)n > KILN_CAP_DEFAULT) return %s_fail(out, KILN_ERR_LIMIT);\n", cur_struct);
    fprintf(f, "            if (pos + (size_t)n > len) return %s_fail(out, KILN_ERR_EOF);\n", cur_struct);
    if (!binary) {
        fprintf(f, "            if (!kiln_utf8_ok(src + pos, n)) return %s_fail(out, KILN_ERR_UTF8);\n", cur_struct);
    }
    fprintf(f, "            %s = NULL;\n            %s = n;\n", ptr, len_field);
    fprintf(f, "            if (n) {\n                %s = malloc(n + 1u);\n", ptr);
    fprintf(f, "                if (!%s) return %s_fail(out, KILN_ERR_LIMIT);\n", ptr, cur_struct);
    fprintf(f, "                memcpy(%s, src + pos, n);\n                %s[n] = 0;\n            }\n", ptr, ptr);
    fprintf(f, "            pos += n;\n        }\n");
}

static void emit_decode_field(FILE *f, const char *parent, const GField *field) {
    const GType *t = field->type;
    char dest[160];
    (void)parent;
    if (!field->required) {
        fprintf(f, "    {\n        uint8_t tag;\n        if (pos + 1 > len) return %s_fail(out, KILN_ERR_EOF);\n", cur_struct);
        fprintf(f, "        tag = src[pos++];\n        if (tag == 0) {\n            out->%s_present = 0;\n", field->name);
        fprintf(f, "        } else if (tag == 1) {\n            out->%s_present = 1;\n", field->name);
    }
    if (scalar_width(t->kind)) {
        snprintf(dest, sizeof dest, "out->%s", field->name);
        emit_decode_scalar(f, t->kind, dest);
    } else if (t->kind == GK_STR || t->kind == GK_BYTES) {
        char ptr[128];
        char ln[128];
        snprintf(ptr, sizeof ptr, "out->%s", field->name);
        snprintf(ln, sizeof ln, "out->%s_len", field->name);
        emit_read_text(f, ptr, ln, t->kind == GK_BYTES);
    } else if (t->kind == GK_STRUCT) {
        fprintf(f, "        {\n            size_t used = 0;\n            kiln_err err = %s_decode_into(&out->%s, src + pos, len - pos, &used);\n",
            t->name, field->name);
        fprintf(f, "            if (err) return %s_fail(out, err);\n            pos += used;\n        }\n", cur_struct);
    } else if (t->kind == GK_LIST) {
        size_t min = type_min(t->elem);
        int ew = scalar_width(t->elem->kind);
        fprintf(f, "        {\n            uint32_t count;\n            size_t i;\n");
        fprintf(f, "            if (pos + 4 > len) return %s_fail(out, KILN_ERR_EOF);\n", cur_struct);
        fprintf(f, "            count = kiln_load_u32(src + pos);\n            pos += 4;\n");
        fprintf(f, "            if ((uint64_t)count > KILN_CAP_DEFAULT) return %s_fail(out, KILN_ERR_LIMIT);\n", cur_struct);
        if (min > 0) {
            fprintf(f, "            if ((uint64_t)count > (len - pos) / %zuu) return %s_fail(out, KILN_ERR_EOF);\n", min, cur_struct);
        }
        fprintf(f, "            out->%s = NULL;\n            out->%s_count = count;\n", field->name, field->name);
        fprintf(f, "            if (count) {\n                out->%s = calloc(count, sizeof *out->%s);\n", field->name, field->name);
        fprintf(f, "                if (!out->%s) { out->%s_count = 0; return %s_fail(out, KILN_ERR_LIMIT); }\n", field->name, field->name, cur_struct);
        fprintf(f, "            }\n            for (i = 0; i < count; i++) {\n");
        if (ew) {
            snprintf(dest, sizeof dest, "out->%s[i]", field->name);
            emit_decode_scalar(f, t->elem->kind, dest);
        } else if (t->elem->kind == GK_STR || t->elem->kind == GK_BYTES) {
            char ptr[160];
            char ln[160];
            snprintf(ptr, sizeof ptr, "out->%s[i].data", field->name);
            snprintf(ln, sizeof ln, "out->%s[i].len", field->name);
            emit_read_text(f, ptr, ln, t->elem->kind == GK_BYTES);
        } else if (t->elem->kind == GK_STRUCT) {
            fprintf(f, "                size_t used = 0;\n                kiln_err err = %s_decode_into(&out->%s[i], src + pos, len - pos, &used);\n",
                t->elem->name, field->name);
            fprintf(f, "                if (err) return %s_fail(out, err);\n                pos += used;\n", cur_struct);
        } else {
            die("cannot decode list element");
        }
        fprintf(f, "            }\n        }\n");
    } else {
        die("cannot decode field");
    }
    if (!field->required) {
        fprintf(f, "        } else {\n            return %s_fail(out, KILN_ERR_TYPE);\n        }\n    }\n", cur_struct);
    }
}

static void emit_source(FILE *f, const char *stem) {
    size_t s;
    fprintf(f, "/* Generated by gld-kilngen-c. Do not edit. */\n");
    fprintf(f, "#include \"%s.h\"\n\n", stem);
    fprintf(f, "#include <stdlib.h>\n#include <string.h>\n\n");
    for (s = 0; s < ngstructs; s++) {
        const GType *st = gstructs[s];
        size_t i;
        fprintf(f, "void %s_free(struct %s *v) {\n", st->name, st->name);
        fprintf(f, "    if (!v) return;\n");
        for (i = 0; i < st->nfields; i++) {
            emit_free_field(f, st->name, &st->fields[i]);
        }
        fprintf(f, "}\n\n");
        cur_struct = st->name;
        fprintf(f, "static kiln_err %s_fail(struct %s *out, kiln_err err) {\n", st->name, st->name);
        fprintf(f, "    %s_free(out);\n    memset(out, 0, sizeof *out);\n    return err;\n}\n\n", st->name);
        fprintf(f, "size_t %s_encoded_size(const struct %s *v) {\n    size_t n = 0;\n", st->name, st->name);
        for (i = 0; i < st->nfields; i++) {
            emit_size_field(f, &st->fields[i]);
        }
        fprintf(f, "    return n;\n}\n\n");
        fprintf(f, "kiln_err %s_encode(const struct %s *v, uint8_t *dst, size_t cap, size_t *written) {\n", st->name, st->name);
        fprintf(f, "    size_t pos = 0;\n    size_t need = %s_encoded_size(v);\n", st->name);
        fprintf(f, "    if (need > cap) return KILN_ERR_LIMIT;\n");
        for (i = 0; i < st->nfields; i++) {
            emit_encode_field2(f, &st->fields[i]);
        }
        fprintf(f, "    if (written) *written = pos;\n    return KILN_OK;\n}\n\n");
        fprintf(f, "kiln_err %s_decode_into(struct %s *out, const uint8_t *src, size_t len, size_t *used) {\n",
            st->name, st->name);
        fprintf(f, "    size_t pos = 0;\n    memset(out, 0, sizeof *out);\n");
        for (i = 0; i < st->nfields; i++) {
            emit_decode_field(f, st->name, &st->fields[i]);
        }
        fprintf(f, "    if (used) *used = pos;\n    return KILN_OK;\n}\n\n");
        fprintf(f, "kiln_err %s_decode(struct %s *out, const uint8_t *src, size_t len) {\n", st->name, st->name);
        fprintf(f, "    size_t used = 0;\n    kiln_err err = %s_decode_into(out, src, len, &used);\n", st->name);
        fprintf(f, "    if (err) return err;\n    if (used != len) return %s_fail(out, KILN_ERR_TRAILING);\n    return KILN_OK;\n}\n\n", st->name);
    }
    (void)emit_encode_field;
    (void)failed;
}

static const char *stem_of(const char *path, char *buf, size_t n) {
    const char *base = strrchr(path, '/');
    const char *dot;
    size_t len;
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len + 1 > n || len == 0) die("bad schema file name");
    memcpy(buf, base, len);
    buf[len] = 0;
    return buf;
}

int main(int argc, char **argv) {
    const char *schema_path = NULL;
    const char *out_dir = NULL;
    int i;
    size_t len = 0;
    char *text;
    const char *jerr = NULL;
    kiln_json *root;
    const kiln_json *title;
    const kiln_json *defs;
    char stem[128];
    char path[512];
    FILE *hf;
    FILE *cf;
    size_t s;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--schema") == 0 && i + 1 < argc) {
            schema_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else {
            fprintf(stderr, "usage: gld-kilngen-c --schema FILE --out DIR\n");
            return 2;
        }
    }
    if (!schema_path || !out_dir) {
        fprintf(stderr, "usage: gld-kilngen-c --schema FILE --out DIR\n");
        return 2;
    }
    text = read_all(schema_path, &len);
    root = kiln_json_parse(text, len, &jerr);
    if (!root) {
        fprintf(stderr, "json: %s\n", jerr ? jerr : "parse error");
        return 1;
    }
    title = kiln_json_get(root, "title");
    if (!title || title->kind != KILN_JSON_STR) die("schema needs a title");
    new_struct(title->str);
    defs = kiln_json_get(root, "$defs");
    if (defs) {
        if (defs->kind != KILN_JSON_OBJ) die("$defs");
        for (s = 0; s < defs->n; s++) new_struct(defs->keys[s]);
    }
    fill_struct(find_struct(title->str), root, root);
    if (defs) {
        for (s = 0; s < defs->n; s++) {
            fill_struct(find_struct(defs->keys[s]), root, &defs->vals[s]);
        }
    }
    stem_of(schema_path, stem, sizeof stem);
    snprintf(path, sizeof path, "%s/%s.h", out_dir, stem);
    hf = fopen(path, "wb");
    if (!hf) die("cannot write header");
    emit_header2(hf, stem);
    fclose(hf);
    snprintf(path, sizeof path, "%s/%s.c", out_dir, stem);
    cf = fopen(path, "wb");
    if (!cf) die("cannot write source");
    emit_source(cf, stem);
    fclose(cf);
    kiln_json_free(root);
    free(text);
    return 0;
}
