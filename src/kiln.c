#include "kiln.h"
#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_alloc_total;
static int g_count;

void kiln_count_begin(void) {
    g_alloc_total = 0;
    g_count = 1;
}

void kiln_count_end(void) {
    g_count = 0;
}

uint64_t kiln_debug_alloc_total(void) {
    return g_alloc_total;
}

static void *cmalloc(size_t n) {
    void *p;
    if (n == 0) {
        n = 1;
    }
    p = malloc(n);
    if (p && g_count) {
        g_alloc_total += n;
    }
    return p;
}

const char *kiln_version(void) {
    return KILN_VERSION;
}

const char *kiln_err_name(kiln_err err) {
    switch (err) {
    case KILN_OK:
        return "OK";
    case KILN_ERR_FRAME:
        return "FRAME";
    case KILN_ERR_LIMIT:
        return "LIMIT";
    case KILN_ERR_EOF:
        return "EOF";
    case KILN_ERR_TYPE:
        return "TYPE";
    case KILN_ERR_UTF8:
        return "UTF8";
    case KILN_ERR_TRAILING:
        return "TRAILING";
    case KILN_ERR_SCHEMA:
        return "SCHEMA";
    }
    return "TYPE";
}

const char *kiln_dtype_name(uint16_t code) {
    static const char *names[] = {
        NULL, "f64", "f32", "f16", "bf16", "f8e4m3", "f8e5m2", "bool", "i8", "u8", "i16", "u16", "i32",
        "u32", "i64", "u64", "i4", "u4", "e2m1", "e8m0"};
    if (code >= 1 && code <= 19) {
        return names[code];
    }
    return NULL;
}

struct kiln_chunk {
    struct kiln_chunk *next;
    size_t cap;
    size_t used;
    uint8_t *data;
};

struct kiln_arena {
    struct kiln_chunk *head;
};

kiln_arena *kiln_arena_new(void) {
    kiln_arena *arena = cmalloc(sizeof(*arena));
    if (arena) {
        memset(arena, 0, sizeof(*arena));
    }
    return arena;
}

void kiln_arena_reset(kiln_arena *arena) {
    struct kiln_chunk *extra;
    if (!arena || !arena->head) {
        return;
    }
    extra = arena->head->next;
    arena->head->next = NULL;
    arena->head->used = 0;
    while (extra) {
        struct kiln_chunk *next = extra->next;
        free(extra->data);
        free(extra);
        extra = next;
    }
}

void kiln_arena_free(kiln_arena *arena) {
    struct kiln_chunk *c;
    if (!arena) {
        return;
    }
    c = arena->head;
    while (c) {
        struct kiln_chunk *n = c->next;
        free(c->data);
        free(c);
        c = n;
    }
    free(arena);
}

void *kiln_arena_alloc(kiln_arena *arena, size_t size, size_t align) {
    struct kiln_chunk *c;
    size_t off;
    size_t cap;
    if (!arena) {
        return NULL;
    }
    if (align == 0 || (align & (align - 1)) != 0) {
        align = 8;
    }
    if (size > SIZE_MAX - align) {
        return NULL;
    }
    c = arena->head;
    if (c) {
        off = (c->used + (align - 1)) & ~(align - 1);
        if (off <= c->cap && size <= c->cap - off) {
            c->used = off + size;
            return c->data + off;
        }
    }
    cap = size + align;
    if (cap < 4096) {
        cap = 4096;
    }
    c = cmalloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->data = cmalloc(cap);
    if (!c->data) {
        free(c);
        if (g_count && g_alloc_total >= sizeof(*c)) {
            g_alloc_total -= sizeof(*c);
        }
        return NULL;
    }
    c->cap = cap;
    c->used = 0;
    c->next = arena->head;
    arena->head = c;
    off = (align - 1) & ~(align - 1);
    c->used = off + size;
    return c->data + off;
}

char *kiln_arena_str(kiln_arena *arena, const char *s, size_t n) {
    char *d = kiln_arena_alloc(arena, n + 1, 1);
    if (!d) {
        return NULL;
    }
    if (n) {
        memcpy(d, s, n);
    }
    d[n] = 0;
    return d;
}

kiln_schema *kiln_schema_new(kiln_arena *arena, kiln_kind kind) {
    kiln_schema *s = kiln_arena_alloc(arena, sizeof(*s), sizeof(void *));
    if (!s) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    return s;
}

int kiln_utf8_ok(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t need;
        uint32_t cp;
        size_t j;
        if (c <= 0x7F) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2) {
                return 0;
            }
            need = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            if (c > 0xF4) {
                return 0;
            }
            need = 4;
            cp = c & 0x07u;
        } else {
            return 0;
        }
        if (i + need > n) {
            return 0;
        }
        for (j = 1; j < need; j++) {
            if ((s[i + j] & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (s[i + j] & 0x3Fu);
        }
        if (need == 3) {
            if (s[i] == 0xE0 && s[i + 1] < 0xA0) {
                return 0;
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                return 0;
            }
        } else if (need == 4) {
            if (s[i] == 0xF0 && s[i + 1] < 0x90) {
                return 0;
            }
            if (s[i] == 0xF4 && s[i + 1] > 0x8F) {
                return 0;
            }
            if (cp > 0x10FFFF) {
                return 0;
            }
        }
        i += need;
    }
    return 1;
}

static uint64_t mix(uint64_t h, const char *bytes, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)bytes[i];
        h *= 1099511628211ULL;
    }
    h *= 1099511628211ULL;
    return h;
}

static uint64_t hash_schema(uint64_t h, const kiln_schema *schema) {
    const char *tok = NULL;
    size_t i;
    switch (schema->kind) {
    case KILN_BOOL:
        tok = "bool";
        break;
    case KILN_I8:
        tok = "i8";
        break;
    case KILN_I16:
        tok = "i16";
        break;
    case KILN_I32:
        tok = "i32";
        break;
    case KILN_I64:
        tok = "i64";
        break;
    case KILN_INT:
        tok = "int";
        break;
    case KILN_U8:
        tok = "u8";
        break;
    case KILN_U16:
        tok = "u16";
        break;
    case KILN_U32:
        tok = "u32";
        break;
    case KILN_U64:
        tok = "u64";
        break;
    case KILN_UINT:
        tok = "uint";
        break;
    case KILN_F32:
        tok = "f32";
        break;
    case KILN_F64:
        tok = "f64";
        break;
    case KILN_STR:
        tok = "str";
        break;
    case KILN_BYTES:
        tok = "bytes";
        break;
    case KILN_TENSORS:
        tok = "tensors";
        break;
    case KILN_LIST:
        h = mix(h, "list<", 5);
        h = hash_schema(h, schema->elem);
        return mix(h, ">", 1);
    case KILN_OPT:
        h = mix(h, "opt<", 4);
        h = hash_schema(h, schema->elem);
        return mix(h, ">", 1);
    case KILN_DICT:
        h = mix(h, "dict<str,", 9);
        h = hash_schema(h, schema->elem);
        return mix(h, ">", 1);
    case KILN_STRUCT:
        h = mix(h, "struct{", 7);
        for (i = 0; i < schema->nfields; i++) {
            const char *name = schema->fields[i].name;
            h = mix(h, name, strlen(name));
            h = mix(h, ":", 1);
            h = hash_schema(h, schema->fields[i].type);
            h = mix(h, ",", 1);
        }
        return mix(h, "}", 1);
    }
    if (!tok) {
        return h;
    }
    return mix(h, tok, strlen(tok));
}

uint64_t kiln_type_hash(const kiln_schema *schema) {
    return hash_schema(14695981039346656037ULL, schema);
}

size_t kiln_min_size(const kiln_schema *schema) {
    size_t i;
    size_t n;
    switch (schema->kind) {
    case KILN_BOOL:
    case KILN_I8:
    case KILN_U8:
        return 1;
    case KILN_I16:
    case KILN_U16:
        return 2;
    case KILN_I32:
    case KILN_U32:
    case KILN_F32:
        return 4;
    case KILN_I64:
    case KILN_U64:
    case KILN_INT:
    case KILN_UINT:
    case KILN_F64:
        return 8;
    case KILN_STR:
    case KILN_BYTES:
        return 4;
    case KILN_TENSORS:
        return 64;
    case KILN_LIST:
    case KILN_DICT:
        return 4;
    case KILN_OPT:
        return 1;
    case KILN_STRUCT:
        n = 0;
        for (i = 0; i < schema->nfields; i++) {
            n += kiln_min_size(schema->fields[i].type);
        }
        return n;
    }
    return 0;
}

int kiln_fixed_size(const kiln_schema *schema, size_t *size) {
    size_t i;
    size_t n = 0;
    size_t part;
    switch (schema->kind) {
    case KILN_BOOL:
    case KILN_I8:
    case KILN_U8:
        *size = 1;
        return 1;
    case KILN_I16:
    case KILN_U16:
        *size = 2;
        return 1;
    case KILN_I32:
    case KILN_U32:
    case KILN_F32:
        *size = 4;
        return 1;
    case KILN_I64:
    case KILN_INT:
    case KILN_U64:
    case KILN_UINT:
    case KILN_F64:
        *size = 8;
        return 1;
    case KILN_STRUCT:
        for (i = 0; i < schema->nfields; i++) {
            if (!kiln_fixed_size(schema->fields[i].type, &part)) {
                return 0;
            }
            n += part;
        }
        *size = n;
        return 1;
    default:
        return 0;
    }
}

void kiln_tensor_clear(kiln_tensor *tensor) {
    memset(tensor, 0, sizeof(*tensor));
    tensor->quant_axis = 0xFFFF;
    tensor->scale_index = 0xFFFFFFFFu;
    tensor->outer_index = 0xFFFFFFFFu;
}

void kiln_buf_init(kiln_buf *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void kiln_buf_free(kiln_buf *buf) {
    free(buf->data);
    kiln_buf_init(buf);
}

void kiln_buf_reset(kiln_buf *buf) {
    buf->len = 0;
}

kiln_err kiln_buf_reserve(kiln_buf *buf, size_t n) {
    size_t need;
    size_t nc;
    uint8_t *p;
    if (n > SIZE_MAX - buf->len) {
        return KILN_ERR_LIMIT;
    }
    need = buf->len + n;
    if (need <= buf->cap) {
        return KILN_OK;
    }
    nc = buf->cap ? buf->cap : 64;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) {
            return KILN_ERR_LIMIT;
        }
        nc *= 2;
    }
    p = realloc(buf->data, nc);
    if (!p) {
        return KILN_ERR_LIMIT;
    }
    buf->data = p;
    buf->cap = nc;
    return KILN_OK;
}

typedef struct {
    const char *s;
    size_t n;
    size_t i;
    kiln_arena *arena;
} SP;

static void sp_skip(SP *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->i++;
    }
}

static int sp_eat(SP *p, char c) {
    sp_skip(p);
    if (p->i < p->n && p->s[p->i] == c) {
        p->i++;
        return 1;
    }
    return 0;
}

static int sp_ident(SP *p, char **out) {
    size_t start;
    size_t n;
    sp_skip(p);
    if (p->i >= p->n) {
        return 0;
    }
    if (!isalpha((unsigned char)p->s[p->i]) && p->s[p->i] != '_') {
        return 0;
    }
    start = p->i;
    p->i++;
    while (p->i < p->n && (isalnum((unsigned char)p->s[p->i]) || p->s[p->i] == '_')) {
        p->i++;
    }
    n = p->i - start;
    *out = kiln_arena_str(p->arena, p->s + start, n);
    return *out != NULL;
}

static kiln_err sp_type(SP *p, kiln_schema **out);

static kiln_kind scalar_kind(const char *name) {
    if (strcmp(name, "bool") == 0) return KILN_BOOL;
    if (strcmp(name, "i8") == 0) return KILN_I8;
    if (strcmp(name, "i16") == 0) return KILN_I16;
    if (strcmp(name, "i32") == 0) return KILN_I32;
    if (strcmp(name, "i64") == 0) return KILN_I64;
    if (strcmp(name, "int") == 0) return KILN_INT;
    if (strcmp(name, "u8") == 0) return KILN_U8;
    if (strcmp(name, "u16") == 0) return KILN_U16;
    if (strcmp(name, "u32") == 0) return KILN_U32;
    if (strcmp(name, "u64") == 0) return KILN_U64;
    if (strcmp(name, "uint") == 0) return KILN_UINT;
    if (strcmp(name, "f32") == 0) return KILN_F32;
    if (strcmp(name, "f64") == 0) return KILN_F64;
    if (strcmp(name, "str") == 0) return KILN_STR;
    if (strcmp(name, "bytes") == 0) return KILN_BYTES;
    if (strcmp(name, "tensors") == 0) return KILN_TENSORS;
    return 0;
}

static kiln_err sp_type(SP *p, kiln_schema **out) {
    char *name = NULL;
    kiln_kind kind;
    if (!sp_ident(p, &name)) {
        return KILN_ERR_SCHEMA;
    }
    kind = scalar_kind(name);
    if (kind) {
        kiln_schema *s = kiln_schema_new(p->arena, kind);
        if (!s) {
            return KILN_ERR_LIMIT;
        }
        *out = s;
        return KILN_OK;
    }
    if (strcmp(name, "list") == 0 || strcmp(name, "opt") == 0) {
        kiln_schema *elem = NULL;
        kiln_schema *s;
        kiln_err err;
        if (!sp_eat(p, '<')) {
            return KILN_ERR_SCHEMA;
        }
        err = sp_type(p, &elem);
        if (err) {
            return err;
        }
        if (!sp_eat(p, '>')) {
            return KILN_ERR_SCHEMA;
        }
        s = kiln_schema_new(p->arena, strcmp(name, "list") == 0 ? KILN_LIST : KILN_OPT);
        if (!s) {
            return KILN_ERR_LIMIT;
        }
        s->elem = elem;
        *out = s;
        return KILN_OK;
    }
    if (strcmp(name, "dict") == 0) {
        char *keyname = NULL;
        kiln_schema *elem = NULL;
        kiln_schema *s;
        kiln_err err;
        if (!sp_eat(p, '<')) {
            return KILN_ERR_SCHEMA;
        }
        if (!sp_ident(p, &keyname) || strcmp(keyname, "str") != 0 || !sp_eat(p, ',')) {
            return KILN_ERR_SCHEMA;
        }
        err = sp_type(p, &elem);
        if (err) {
            return err;
        }
        if (!sp_eat(p, '>')) {
            return KILN_ERR_SCHEMA;
        }
        s = kiln_schema_new(p->arena, KILN_DICT);
        if (!s) {
            return KILN_ERR_LIMIT;
        }
        s->elem = elem;
        *out = s;
        return KILN_OK;
    }
    if (strcmp(name, "struct") == 0) {
        kiln_field *fields = NULL;
        size_t n = 0;
        size_t cap = 0;
        kiln_schema *s;
        if (!sp_eat(p, '{')) {
            return KILN_ERR_SCHEMA;
        }
        sp_skip(p);
        if (!sp_eat(p, '}')) {
            for (;;) {
                char *fname = NULL;
                kiln_schema *ft = NULL;
                kiln_err err;
                kiln_field *nf;
                if (!sp_ident(p, &fname) || !sp_eat(p, ':')) {
                    free(fields);
                    return KILN_ERR_SCHEMA;
                }
                err = sp_type(p, &ft);
                if (err) {
                    free(fields);
                    return err;
                }
                if (n + 1 > cap) {
                    size_t nc = cap ? cap * 2 : 4;
                    nf = realloc(fields, nc * sizeof(*nf));
                    if (!nf) {
                        free(fields);
                        return KILN_ERR_LIMIT;
                    }
                    fields = nf;
                    cap = nc;
                }
                fields[n].name = fname;
                fields[n].type = ft;
                n++;
                sp_skip(p);
                if (sp_eat(p, ',')) {
                    sp_skip(p);
                    if (p->i < p->n && p->s[p->i] == '}') {
                        p->i++;
                        break;
                    }
                    continue;
                }
                if (sp_eat(p, '}')) {
                    break;
                }
                free(fields);
                return KILN_ERR_SCHEMA;
            }
        }
        s = kiln_schema_new(p->arena, KILN_STRUCT);
        if (!s) {
            free(fields);
            return KILN_ERR_LIMIT;
        }
        if (n) {
            kiln_field *copy = kiln_arena_alloc(p->arena, n * sizeof(*copy), sizeof(void *));
            if (!copy) {
                free(fields);
                return KILN_ERR_LIMIT;
            }
            memcpy(copy, fields, n * sizeof(*copy));
            s->fields = copy;
            s->nfields = n;
        }
        free(fields);
        *out = s;
        return KILN_OK;
    }
    return KILN_ERR_SCHEMA;
}

kiln_err kiln_schema_parse(kiln_arena *arena, const char *text, kiln_schema **out) {
    SP p;
    kiln_err err;
    p.s = text;
    p.n = strlen(text);
    p.i = 0;
    p.arena = arena;
    err = sp_type(&p, out);
    if (err) {
        return err;
    }
    sp_skip(&p);
    if (p.i != p.n) {
        return KILN_ERR_SCHEMA;
    }
    return KILN_OK;
}
