#include "kiln.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const kiln_schema SCH_I64 = {KILN_I64, NULL, NULL, 0};
static const kiln_schema SCH_TENSORS = {KILN_TENSORS, NULL, NULL, 0};
static const kiln_field FRAME_FIELDS[] = {
    {"id", &SCH_I64},
    {"batch", &SCH_TENSORS},
};
static const kiln_schema SCH_FRAME = {KILN_STRUCT, NULL, FRAME_FIELDS, 2};

static int fail_n;

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static uint8_t *hex_decode(const char *hex, size_t *out_len) {
    size_t n = strlen(hex);
    size_t i;
    uint8_t *buf;
    if (n % 2) {
        return NULL;
    }
    buf = malloc(n / 2 + 1);
    if (!buf) {
        return NULL;
    }
    for (i = 0; i < n; i += 2) {
        int hi = -1;
        int lo = -1;
        char a = hex[i];
        char b = hex[i + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) {
            free(buf);
            return NULL;
        }
        buf[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return buf;
}

static int hex_eq_bytes(const uint8_t *p, size_t n, const char *hex) {
    size_t i;
    if (strlen(hex) != n * 2) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        int hi = -1;
        int lo = -1;
        char a = hex[i * 2];
        char b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        if (hi < 0 || lo < 0 || p[i] != (uint8_t)((hi << 4) | lo)) {
            return 0;
        }
    }
    return 1;
}

static int json_int(const kiln_json *j, int64_t *out) {
    char *end = NULL;
    long long v;
    if (!j || j->kind != KILN_JSON_NUM || strpbrk(j->str, ".eE")) {
        return 0;
    }
    errno = 0;
    v = strtoll(j->str, &end, 10);
    if (errno || !end || *end) {
        return 0;
    }
    *out = (int64_t)v;
    return 1;
}

static int json_u64(const kiln_json *j, uint64_t *out) {
    char *end = NULL;
    unsigned long long v;
    if (!j || j->kind != KILN_JSON_NUM || j->str[0] == '-' || strpbrk(j->str, ".eE")) {
        return 0;
    }
    errno = 0;
    v = strtoull(j->str, &end, 10);
    if (errno || !end || *end) {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

static int json_f64(const kiln_json *j, double *out) {
    char *end = NULL;
    double v;
    if (!j || j->kind != KILN_JSON_NUM) {
        return 0;
    }
    errno = 0;
    v = strtod(j->str, &end);
    if (errno || !end || *end) {
        return 0;
    }
    *out = v;
    return 1;
}

static const char *match_raw(const kiln_schema *schema, const uint8_t *p, const kiln_json *j);
static const char *match_val(const kiln_schema *schema, const kiln_value *v, const kiln_json *j);

static const char *match_tensors(const kiln_value *v, const kiln_json *j) {
    const kiln_json *version;
    const kiln_json *flags;
    const kiln_json *tensors;
    const kiln_json *block;
    uint64_t num;
    size_t i;
    if (j->kind != KILN_JSON_OBJ) {
        return "tensor object";
    }
    version = kiln_json_get(j, "version");
    flags = kiln_json_get(j, "flags");
    tensors = kiln_json_get(j, "tensors");
    block = kiln_json_get(j, "block_length");
    if (!version || !json_u64(version, &num) || num != 2) {
        return "version";
    }
    if (!block || !json_u64(block, &num) || num != v->as.tensors.block_length) {
        return "block_length";
    }
    if (!flags || flags->kind != KILN_JSON_ARR) {
        return "flags";
    }
    if (v->as.tensors.names) {
        if (flags->n != 2 || strcmp(flags->vals[0].str, "names") != 0 || strcmp(flags->vals[1].str, "le") != 0) {
            return "flags names";
        }
    } else if (flags->n != 1 || strcmp(flags->vals[0].str, "le") != 0) {
        return "flags le";
    }
    if (!tensors || tensors->kind != KILN_JSON_ARR || tensors->n != v->as.tensors.count) {
        return "tensor count";
    }
    for (i = 0; i < v->as.tensors.count; i++) {
        const kiln_tensor *t = &v->as.tensors.items[i];
        const kiln_json *item = &tensors->vals[i];
        const kiln_json *name;
        const kiln_json *dtype;
        const kiln_json *shape;
        const kiln_json *quant;
        const kiln_json *data;
        const char *dname;
        size_t d;
        if (item->kind != KILN_JSON_OBJ) {
            return "tensor item";
        }
        name = kiln_json_get(item, "name");
        dtype = kiln_json_get(item, "dtype");
        shape = kiln_json_get(item, "shape");
        quant = kiln_json_get(item, "quant_scheme");
        data = kiln_json_get(item, "data_hex");
        if (!name) {
            return "name missing";
        }
        if (t->name_len == 0) {
            if (name->kind != KILN_JSON_NULL) {
                return "name";
            }
        } else if (name->kind != KILN_JSON_STR || name->n != t->name_len ||
                   memcmp(name->str, t->name, t->name_len) != 0) {
            return "name";
        }
        dname = kiln_dtype_name(t->dtype);
        if (!dtype) {
            return "dtype missing";
        }
        if (dtype->kind == KILN_JSON_STR) {
            if (!dname || strcmp(dtype->str, dname) != 0) {
                return "dtype";
            }
        } else if (!json_u64(dtype, &num) || num != t->dtype) {
            return "dtype code";
        }
        if (!shape || shape->kind != KILN_JSON_ARR || shape->n != t->rank) {
            return "shape";
        }
        for (d = 0; d < t->rank; d++) {
            if (!json_u64(&shape->vals[d], &num) || num != t->shape[d]) {
                return "dim";
            }
        }
        if (!quant || !json_u64(quant, &num) || num != t->quant_scheme) {
            return "quant";
        }
        if (!data || data->kind != KILN_JSON_STR || !hex_eq_bytes(t->data, t->data_len, data->str)) {
            return "data_hex";
        }
    }
    return NULL;
}

static const char *match_raw(const kiln_schema *schema, const uint8_t *p, const kiln_json *j) {
    int64_t si;
    uint64_t ui;
    double fd;
    size_t width = 0;
    size_t off = 0;
    size_t i;
    if (schema->kind == KILN_STRUCT) {
        if (j->kind != KILN_JSON_OBJ || j->n != schema->nfields) {
            return "raw struct";
        }
        for (i = 0; i < schema->nfields; i++) {
            const kiln_json *field = kiln_json_get(j, schema->fields[i].name);
            const char *err;
            size_t part = 0;
            if (!field) {
                return "raw field";
            }
            err = match_raw(schema->fields[i].type, p + off, field);
            if (err) {
                return err;
            }
            if (!kiln_fixed_size(schema->fields[i].type, &part)) {
                return "raw not fixed";
            }
            off += part;
        }
        return NULL;
    }
    if (!kiln_fixed_size(schema, &width)) {
        return "raw kind";
    }
    switch (schema->kind) {
    case KILN_BOOL:
        if (j->kind != KILN_JSON_BOOL || j->b != p[0]) {
            return "bool";
        }
        return NULL;
    case KILN_I8:
        if (!json_int(j, &si) || si != kiln_load_i8(p)) return "i8";
        return NULL;
    case KILN_I16:
        if (!json_int(j, &si) || si != kiln_load_i16(p)) return "i16";
        return NULL;
    case KILN_I32:
        if (!json_int(j, &si) || si != kiln_load_i32(p)) return "i32";
        return NULL;
    case KILN_I64:
    case KILN_INT:
        if (!json_int(j, &si) || si != kiln_load_i64(p)) return "i64";
        return NULL;
    case KILN_U8:
        if (!json_u64(j, &ui) || ui != p[0]) return "u8";
        return NULL;
    case KILN_U16:
        if (!json_u64(j, &ui) || ui != kiln_load_u16(p)) return "u16";
        return NULL;
    case KILN_U32:
        if (!json_u64(j, &ui) || ui != kiln_load_u32(p)) return "u32";
        return NULL;
    case KILN_U64:
    case KILN_UINT:
        if (!json_u64(j, &ui) || ui != kiln_load_u64(p)) return "u64";
        return NULL;
    case KILN_F32:
        if (!json_f64(j, &fd) || fd != (double)kiln_load_f32(p)) return "f32";
        return NULL;
    case KILN_F64:
        if (!json_f64(j, &fd) || fd != kiln_load_f64(p)) return "f64";
        return NULL;
    default:
        (void)width;
        return "raw scalar";
    }
}

static const char *match_val(const kiln_schema *schema, const kiln_value *v, const kiln_json *j) {
    int64_t si;
    uint64_t ui;
    double fd;
    size_t i;
    if (schema->kind == KILN_TENSORS || v->kind == KILN_TENSORS) {
        return match_tensors(v, j);
    }
    switch (schema->kind) {
    case KILN_BOOL:
        if (j->kind != KILN_JSON_BOOL || j->b != v->as.b) return "bool";
        return NULL;
    case KILN_I8:
    case KILN_I16:
    case KILN_I32:
    case KILN_I64:
    case KILN_INT:
        if (!json_int(j, &si) || si != v->as.i64) return "int";
        return NULL;
    case KILN_U8:
    case KILN_U16:
    case KILN_U32:
    case KILN_U64:
    case KILN_UINT:
        if (!json_u64(j, &ui) || ui != v->as.u64) return "uint";
        return NULL;
    case KILN_F32:
        if (!json_f64(j, &fd)) return "f32";
        {
            float f;
            memcpy(&f, &v->as.f32_bits, 4);
            if (fd != (double)f) return "f32";
        }
        return NULL;
    case KILN_F64:
        if (!json_f64(j, &fd)) return "f64";
        {
            double f;
            memcpy(&f, &v->as.f64_bits, 8);
            if (fd != f) return "f64";
        }
        return NULL;
    case KILN_STR:
        if (j->kind != KILN_JSON_STR || j->n != v->as.bytes.len ||
            memcmp(j->str, v->as.bytes.ptr, j->n) != 0) {
            return "str";
        }
        return NULL;
    case KILN_LIST:
        if (j->kind != KILN_JSON_ARR || j->n != v->as.list.count) return "list";
        if (v->as.list.bulk) {
            for (i = 0; i < v->as.list.count; i++) {
                const char *err = match_raw(schema->elem, v->as.list.raw + i * v->as.list.elem_size, &j->vals[i]);
                if (err) return err;
            }
            return NULL;
        }
        for (i = 0; i < v->as.list.count; i++) {
            const char *err = match_val(schema->elem, &v->as.list.items[i], &j->vals[i]);
            if (err) return err;
        }
        return NULL;
    case KILN_STRUCT:
        if (j->kind != KILN_JSON_OBJ) return "struct";
        for (i = 0; i < schema->nfields; i++) {
            const kiln_json *field = kiln_json_get(j, schema->fields[i].name);
            const char *err;
            if (!field) return "field";
            err = match_val(schema->fields[i].type, &v->as.strukt.fields[i], field);
            if (err) return err;
        }
        return NULL;
    default:
        return "kind";
    }
}

static int hostile_case(const char *id) {
    return strcmp(id, "kiln-v2-directory-eof") == 0 || strcmp(id, "kiln-v2-count-cap") == 0 ||
           strcmp(id, "kiln-v2-payload-overflow") == 0 || strcmp(id, "kiln-list-struct-hostile") == 0 ||
           strcmp(id, "kiln-list-struct-short") == 0 || strcmp(id, "kiln-list-str-short") == 0 ||
           strcmp(id, "kiln-pod-list-short") == 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int bytes_eq(const uint8_t *a, size_t na, const uint8_t *b, size_t nb, size_t *at) {
    size_t i;
    size_t n = na < nb ? na : nb;
    if (na != nb) {
        *at = n;
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            *at = i;
            return 0;
        }
    }
    return 1;
}

static void dump_hex_at(const uint8_t *buf, size_t n, size_t at) {
    size_t i;
    size_t begin = at > 8 ? at - 8 : 0;
    size_t end = at + 8 < n ? at + 8 : n;
    fprintf(stderr, " around %zu:", at);
    for (i = begin; i < end; i++) {
        fprintf(stderr, " %02x", buf[i]);
    }
    fprintf(stderr, "\n");
}

static const kiln_schema *schema_for(kiln_arena *arena, const kiln_json *schema, int *framed) {
    char text[256];
    kiln_schema *out = NULL;
    *framed = 0;
    if (schema->kind == KILN_JSON_STR) {
        if (strcmp(schema->str, "tensor-v2") == 0) {
            return &SCH_TENSORS;
        }
        if (strcmp(schema->str, "frame") == 0) {
            *framed = 1;
            return &SCH_FRAME;
        }
        die("unknown schema string");
    }
    if (schema->kind == KILN_JSON_OBJ) {
        const kiln_json *type = kiln_json_get(schema, "type");
        if (!type || type->kind != KILN_JSON_STR) {
            die("schema type");
        }
        if (strcmp(type->str, "list") == 0) {
            const kiln_json *elem = kiln_json_get(schema, "element");
            snprintf(text, sizeof text, "list<%s>", elem->str);
        } else if (strcmp(type->str, "struct") == 0) {
            const kiln_json *fields = kiln_json_get(schema, "fields");
            size_t i;
            size_t used = 0;
            int n = snprintf(text, sizeof text, "struct{");
            if (n < 0) die("schema");
            used = (size_t)n;
            for (i = 0; i < fields->n; i++) {
                n = snprintf(text + used, sizeof text - used, "%s%s", fields->vals[i].str, i + 1 == fields->n ? "}" : ",");
                if (n < 0 || (size_t)n >= sizeof text - used) die("schema text");
                used += (size_t)n;
            }
        } else {
            die("schema object");
        }
        if (kiln_schema_parse(arena, text, &out)) {
            fprintf(stderr, "parse schema %s\n", text);
            die("parse");
        }
        return out;
    }
    die("schema kind");
    return NULL;
}

static void expect_err_name(const char *id, kiln_err err, const char *want) {
    const char *got = kiln_err_name(err);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: error %s want %s\n", id, got, want);
        fail_n++;
    }
}

static int run_cases(void) {
    size_t len = 0;
    char *text = read_file("testdata/kiln-v2-cases.json", &len);
    const char *jerr = NULL;
    kiln_json *root;
    const kiln_json *cases;
    size_t i;
    if (!text) {
        die("cannot open testdata/kiln-v2-cases.json");
    }
    root = kiln_json_parse(text, len, &jerr);
    if (!root) {
        fprintf(stderr, "json: %s\n", jerr ? jerr : "?");
        die("json");
    }
    cases = kiln_json_get(root, "cases");
    if (!cases || cases->kind != KILN_JSON_ARR || cases->n != 46) {
        die("expected 46 cases");
    }
    for (i = 0; i < cases->n; i++) {
        const kiln_json *cs = &cases->vals[i];
        const char *id = kiln_json_get(cs, "id")->str;
        const char *expect = kiln_json_get(cs, "expect")->str;
        const char *input = kiln_json_get(cs, "input")->str;
        const kiln_json *schema_j = kiln_json_get(cs, "schema");
        const kiln_json *decoded = kiln_json_get(cs, "decoded");
        const kiln_json *error = kiln_json_get(cs, "error");
        kiln_arena *arena = kiln_arena_new();
        int framed = 0;
        const kiln_schema *schema;
        size_t n = 0;
        uint8_t *buf;
        kiln_doc *doc = NULL;
        kiln_read_opts opts;
        kiln_err err;
        uint64_t hash = 0;
        if (!arena) die("arena");
        schema = schema_for(arena, schema_j, &framed);
        buf = hex_decode(input, &n);
        if (!buf) {
            fprintf(stderr, "%s: bad hex\n", id);
            fail_n++;
            kiln_arena_free(arena);
            continue;
        }
        memset(&opts, 0, sizeof opts);
        opts.standalone = 1;
        opts.cap = 16777216;
        if (framed) {
            err = kiln_decode_framed(schema, buf, n, &opts, &doc, &hash);
        } else {
            err = kiln_decode(schema, buf, n, &opts, &doc);
        }
        if (hostile_case(id) && kiln_debug_alloc_total() != 0) {
            fprintf(stderr, "%s: allocated %llu before reject\n", id, (unsigned long long)kiln_debug_alloc_total());
            fail_n++;
        }
        if (strcmp(expect, "reject") == 0) {
            if (err == KILN_OK) {
                fprintf(stderr, "%s: expected reject\n", id);
                fail_n++;
            } else {
                expect_err_name(id, err, error->str);
            }
            kiln_doc_free(doc);
        } else {
            const char *why;
            uint8_t *enc = NULL;
            size_t en = 0;
            size_t at = 0;
            if (err != KILN_OK) {
                fprintf(stderr, "%s: %s\n", id, kiln_err_name(err));
                fail_n++;
                free(buf);
                kiln_arena_free(arena);
                continue;
            }
            if (framed) {
                const kiln_json *jh = kiln_json_get(decoded, "hash");
                uint64_t want = 0;
                if (!json_u64(jh, &want) || want != hash || hash != kiln_type_hash(schema)) {
                    fprintf(stderr, "%s: hash got %llu\n", id, (unsigned long long)hash);
                    fail_n++;
                }
                why = match_val(&SCH_I64, &doc->root.as.strukt.fields[0], kiln_json_get(decoded, "id"));
                if (why) {
                    fprintf(stderr, "%s: id %s\n", id, why);
                    fail_n++;
                }
                why = match_tensors(&doc->root.as.strukt.fields[1], kiln_json_get(decoded, "batch"));
                if (why) {
                    fprintf(stderr, "%s: batch %s\n", id, why);
                    fail_n++;
                }
                err = kiln_encode_framed(schema, &doc->root, &enc, &en);
            } else {
                if (decoded) {
                    why = match_val(schema, &doc->root, decoded);
                    if (why) {
                        fprintf(stderr, "%s: decoded %s\n", id, why);
                        fail_n++;
                    }
                }
                err = kiln_encode(schema, &doc->root, &enc, &en);
            }
            if (err != KILN_OK) {
                fprintf(stderr, "%s: encode %s\n", id, kiln_err_name(err));
                fail_n++;
            } else if (!bytes_eq(enc, en, buf, n, &at)) {
                fprintf(stderr, "%s: reencode mismatch len %zu vs %zu\n", id, en, n);
                dump_hex_at(buf, n, at);
                dump_hex_at(enc ? enc : (const uint8_t *)"", en, at);
                fail_n++;
            }
            free(enc);
            kiln_doc_free(doc);
        }
        free(buf);
        kiln_arena_free(arena);
    }
    kiln_json_free(root);
    free(text);
    if (fail_n) {
        fprintf(stderr, "%d failure(s)\n", fail_n);
        return 1;
    }
    printf("46 cases passed\n");
    return 0;
}

static void check_hash(void) {
    uint64_t h = kiln_type_hash(&SCH_FRAME);
    if (h != 0xe58aadc7552f0d82ULL) {
        fprintf(stderr, "frame hash %llx\n", (unsigned long long)h);
        die("hash");
    }
}

static void dump_raw(const kiln_schema *s, const uint8_t *p, int depth);
static void dump_val(const kiln_schema *s, const kiln_value *v, int depth);

static void pad(int depth) {
    printf("%d ", depth);
}

static void dump_scalar_raw(const kiln_schema *s, const uint8_t *p) {
    switch (s->kind) {
    case KILN_BOOL:
        printf("BOOL %d\n", p[0]);
        break;
    case KILN_I8:
        printf("I8 %d\n", kiln_load_i8(p));
        break;
    case KILN_I16:
        printf("I16 %d\n", kiln_load_i16(p));
        break;
    case KILN_I32:
        printf("I32 %d\n", kiln_load_i32(p));
        break;
    case KILN_I64:
    case KILN_INT:
        printf("I64 %lld\n", (long long)kiln_load_i64(p));
        break;
    case KILN_U8:
        printf("U8 %u\n", p[0]);
        break;
    case KILN_U16:
        printf("U16 %u\n", kiln_load_u16(p));
        break;
    case KILN_U32:
        printf("U32 %u\n", kiln_load_u32(p));
        break;
    case KILN_U64:
    case KILN_UINT:
        printf("U64 %llu\n", (unsigned long long)kiln_load_u64(p));
        break;
    case KILN_F32:
        printf("F32 %08x\n", kiln_load_u32(p));
        break;
    case KILN_F64:
        printf("F64 %016llx\n", (unsigned long long)kiln_load_u64(p));
        break;
    default:
        printf("RAW\n");
        break;
    }
}

static void dump_raw(const kiln_schema *s, const uint8_t *p, int depth) {
    size_t i;
    size_t off = 0;
    pad(depth);
    if (s->kind == KILN_STRUCT) {
        printf("ST %zu\n", s->nfields);
        for (i = 0; i < s->nfields; i++) {
            size_t part = 0;
            pad(depth + 1);
            printf("%s ", s->fields[i].name);
            dump_scalar_raw(s->fields[i].type, p + off);
            kiln_fixed_size(s->fields[i].type, &part);
            off += part;
        }
        return;
    }
    dump_scalar_raw(s, p);
}

static void dump_bytes(const char *tag, const uint8_t *p, size_t n) {
    size_t i;
    printf("%s %zu ", tag, n);
    for (i = 0; i < n; i++) {
        printf("%02x", p ? p[i] : 0);
    }
    printf("\n");
}

static void dump_val(const kiln_schema *s, const kiln_value *v, int depth) {
    size_t i;
    pad(depth);
    switch (s->kind) {
    case KILN_BOOL:
    case KILN_I8:
    case KILN_I16:
    case KILN_I32:
    case KILN_I64:
    case KILN_INT:
    case KILN_U8:
    case KILN_U16:
    case KILN_U32:
    case KILN_U64:
    case KILN_UINT:
    case KILN_F32:
    case KILN_F64: {
        uint8_t tmp[8];
        size_t width = 0;
        kiln_fixed_size(s, &width);
        if (s->kind == KILN_BOOL) tmp[0] = (uint8_t)v->as.b;
        else if (s->kind == KILN_I8) tmp[0] = (uint8_t)(int8_t)v->as.i64;
        else if (s->kind == KILN_U8) tmp[0] = (uint8_t)v->as.u64;
        else if (s->kind == KILN_I16) kiln_store_u16(tmp, (uint16_t)(int16_t)v->as.i64);
        else if (s->kind == KILN_U16) kiln_store_u16(tmp, (uint16_t)v->as.u64);
        else if (s->kind == KILN_I32) kiln_store_i32(tmp, (int32_t)v->as.i64);
        else if (s->kind == KILN_U32) kiln_store_u32(tmp, (uint32_t)v->as.u64);
        else if (s->kind == KILN_I64 || s->kind == KILN_INT) kiln_store_i64(tmp, v->as.i64);
        else if (s->kind == KILN_U64 || s->kind == KILN_UINT) kiln_store_u64(tmp, v->as.u64);
        else if (s->kind == KILN_F32) kiln_store_u32(tmp, v->as.f32_bits);
        else kiln_store_u64(tmp, v->as.f64_bits);
        dump_scalar_raw(s, tmp);
        (void)width;
        break;
    }
    case KILN_STR:
        dump_bytes("S", v->as.bytes.ptr, v->as.bytes.len);
        break;
    case KILN_BYTES:
        dump_bytes("Y", v->as.bytes.ptr, v->as.bytes.len);
        break;
    case KILN_LIST:
        printf("L %zu\n", v->as.list.count);
        if (v->as.list.bulk) {
            for (i = 0; i < v->as.list.count; i++) {
                dump_raw(s->elem, v->as.list.raw + i * v->as.list.elem_size, depth + 1);
            }
        } else {
            for (i = 0; i < v->as.list.count; i++) {
                dump_val(s->elem, &v->as.list.items[i], depth + 1);
            }
        }
        break;
    case KILN_OPT:
        printf("OPT %d\n", v->as.opt.present);
        if (v->as.opt.present) {
            dump_val(s->elem, v->as.opt.item, depth + 1);
        }
        break;
    case KILN_DICT:
        printf("D %zu\n", v->as.dict.count);
        for (i = 0; i < v->as.dict.count; i++) {
            static const kiln_schema str_schema = {KILN_STR, NULL, NULL, 0};
            dump_val(&str_schema, &v->as.dict.keys[i], depth + 1);
            dump_val(s->elem, &v->as.dict.vals[i], depth + 1);
        }
        break;
    case KILN_STRUCT:
        printf("ST %zu\n", s->nfields);
        for (i = 0; i < s->nfields; i++) {
            pad(depth + 1);
            printf("%s ", s->fields[i].name);
            /* dump_val also prints depth. Print the value on the same conceptual record by calling dump at depth+1 without a second name line. */
            dump_val(s->fields[i].type, &v->as.strukt.fields[i], depth + 1);
        }
        break;
    case KILN_TENSORS:
        printf("T %zu\n", v->as.tensors.count);
        break;
    default:
        printf("?\n");
        break;
    }
}

static int roundtrip_main(int argc, char **argv, int framed) {
    kiln_arena *arena;
    kiln_schema *schema = NULL;
    size_t n = 0;
    uint8_t *buf;
    kiln_doc *doc = NULL;
    kiln_err err;
    uint8_t *enc = NULL;
    size_t en = 0;
    size_t i;
    if (argc != 4) {
        fprintf(stderr, "usage\n");
        return 2;
    }
    arena = kiln_arena_new();
    if (kiln_schema_parse(arena, argv[2], &schema)) {
        fprintf(stderr, "SCHEMA\n");
        return 1;
    }
    buf = hex_decode(argv[3], &n);
    if (!buf) {
        fprintf(stderr, "TYPE\n");
        return 1;
    }
    err = framed ? kiln_decode_framed(schema, buf, n, NULL, &doc, NULL) : kiln_decode(schema, buf, n, NULL, &doc);
    if (err) {
        fprintf(stderr, "%s\n", kiln_err_name(err));
        free(buf);
        kiln_arena_free(arena);
        return 1;
    }
    err = framed ? kiln_encode_framed(schema, &doc->root, &enc, &en) : kiln_encode(schema, &doc->root, &enc, &en);
    if (err) {
        fprintf(stderr, "%s\n", kiln_err_name(err));
        kiln_doc_free(doc);
        free(buf);
        kiln_arena_free(arena);
        return 1;
    }
    for (i = 0; i < en; i++) {
        printf("%02x", enc[i]);
    }
    printf("\n");
    dump_val(schema, &doc->root, 0);
    free(enc);
    kiln_doc_free(doc);
    free(buf);
    kiln_arena_free(arena);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "roundtrip") == 0) {
        return roundtrip_main(argc, argv, 0);
    }
    if (argc >= 2 && strcmp(argv[1], "roundtrip-frame") == 0) {
        return roundtrip_main(argc, argv, 1);
    }
    check_hash();
    return run_cases();
}
