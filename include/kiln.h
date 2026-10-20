#ifndef KILN_H
#define KILN_H

/* From-scratch Kiln wire v2 for C. ISO C11. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_VERSION "0.1.0"
#define KILN_CAP_DEFAULT 16777216u
#define KILN_DEPTH_MAX 64

typedef enum kiln_err {
    KILN_OK = 0,
    KILN_ERR_FRAME = 1,
    KILN_ERR_LIMIT = 2,
    KILN_ERR_EOF = 3,
    KILN_ERR_TYPE = 4,
    KILN_ERR_UTF8 = 5,
    KILN_ERR_TRAILING = 6,
    KILN_ERR_SCHEMA = 7
} kiln_err;

typedef enum kiln_kind {
    KILN_BOOL = 1,
    KILN_I8,
    KILN_I16,
    KILN_I32,
    KILN_I64,
    KILN_INT,
    KILN_U8,
    KILN_U16,
    KILN_U32,
    KILN_U64,
    KILN_UINT,
    KILN_F32,
    KILN_F64,
    KILN_STR,
    KILN_BYTES,
    KILN_LIST,
    KILN_OPT,
    KILN_DICT,
    KILN_STRUCT,
    KILN_TENSORS
} kiln_kind;

typedef struct kiln_schema kiln_schema;
typedef struct kiln_field kiln_field;
typedef struct kiln_arena kiln_arena;
typedef struct kiln_value kiln_value;
typedef struct kiln_tensor kiln_tensor;
typedef struct kiln_doc kiln_doc;

struct kiln_field {
    const char *name;
    const kiln_schema *type;
};

struct kiln_schema {
    kiln_kind kind;
    const kiln_schema *elem;
    const kiln_field *fields;
    size_t nfields;
};

struct kiln_tensor {
    const uint8_t *name;
    size_t name_len;
    const uint8_t *layout;
    size_t layout_len;
    uint16_t dtype;
    uint8_t rank;
    uint16_t quant_scheme;
    uint16_t quant_axis;
    uint32_t group_size;
    uint32_t scale_index;
    uint32_t outer_index;
    uint32_t block_size;
    uint64_t shape[8];
    const uint8_t *data;
    size_t data_len;
};

struct kiln_value {
    kiln_kind kind;
    union {
        int b;
        int64_t i64;
        uint64_t u64;
        uint32_t f32_bits;
        uint64_t f64_bits;
        struct {
            const uint8_t *ptr;
            size_t len;
        } bytes;
        struct {
            kiln_value *items;
            size_t count;
            const uint8_t *raw;
            size_t elem_size;
            int bulk;
        } list;
        struct {
            int present;
            kiln_value *item;
        } opt;
        struct {
            kiln_value *keys;
            kiln_value *vals;
            size_t count;
        } dict;
        struct {
            kiln_value *fields;
            size_t count;
        } strukt;
        struct {
            int names;
            kiln_tensor *items;
            size_t count;
            size_t block_length;
        } tensors;
    } as;
};

struct kiln_doc {
    kiln_arena *arena;
    kiln_value root;
};

typedef struct kiln_read_opts {
    uint64_t cap; /* 0 selects KILN_CAP_DEFAULT */
    int trusted; /* skip UTF-8 and quant-shape checks */
    int standalone; /* reject bytes past the value */
} kiln_read_opts;

typedef struct kiln_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} kiln_buf;

const char *kiln_version(void);
const char *kiln_err_name(kiln_err err);
const char *kiln_dtype_name(uint16_t code);

kiln_arena *kiln_arena_new(void);
void kiln_arena_reset(kiln_arena *arena);
void kiln_arena_free(kiln_arena *arena);
void *kiln_arena_alloc(kiln_arena *arena, size_t size, size_t align);
char *kiln_arena_str(kiln_arena *arena, const char *s, size_t n);

kiln_schema *kiln_schema_new(kiln_arena *arena, kiln_kind kind);
kiln_err kiln_schema_parse(kiln_arena *arena, const char *text, kiln_schema **out);
kiln_err kiln_schema_from_json(kiln_arena *arena, const char *json, size_t len, kiln_schema **out);

uint64_t kiln_type_hash(const kiln_schema *schema);
size_t kiln_min_size(const kiln_schema *schema);
int kiln_fixed_size(const kiln_schema *schema, size_t *size);

void kiln_tensor_clear(kiln_tensor *tensor);

kiln_err kiln_decode(const kiln_schema *schema, const uint8_t *buf, size_t len,
    const kiln_read_opts *opts, kiln_doc **out);
/* Decode into an existing doc, keeping its arena for the next call. */
kiln_err kiln_decode_reuse(kiln_doc *doc, const kiln_schema *schema, const uint8_t *buf, size_t len,
    const kiln_read_opts *opts);
kiln_err kiln_decode_framed(const kiln_schema *schema, const uint8_t *buf, size_t len,
    const kiln_read_opts *opts, kiln_doc **out, uint64_t *hash_out);
void kiln_doc_free(kiln_doc *doc);

void kiln_buf_init(kiln_buf *buf);
void kiln_buf_free(kiln_buf *buf);
void kiln_buf_reset(kiln_buf *buf);
kiln_err kiln_buf_reserve(kiln_buf *buf, size_t n);
kiln_err kiln_encode_buf(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf);
kiln_err kiln_encode(const kiln_schema *schema, const kiln_value *value, uint8_t **out, size_t *len);
kiln_err kiln_encode_framed_buf(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf);
kiln_err kiln_encode_framed(const kiln_schema *schema, const kiln_value *value, uint8_t **out, size_t *len);

int kiln_utf8_ok(const uint8_t *s, size_t n);

/* Bytes allocated by the most recent decode. Hostile counts stay at 0. */
uint64_t kiln_debug_alloc_total(void);

static inline uint16_t kiln_load_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t kiln_load_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t kiln_load_u64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static inline int8_t kiln_load_i8(const uint8_t *p) {
    return (int8_t)p[0];
}

static inline int16_t kiln_load_i16(const uint8_t *p) {
    return (int16_t)kiln_load_u16(p);
}

static inline int32_t kiln_load_i32(const uint8_t *p) {
    return (int32_t)kiln_load_u32(p);
}

static inline int64_t kiln_load_i64(const uint8_t *p) {
    return (int64_t)kiln_load_u64(p);
}

static inline void kiln_store_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void kiln_store_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void kiln_store_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

static inline void kiln_store_i32(uint8_t *p, int32_t v) {
    kiln_store_u32(p, (uint32_t)v);
}

static inline void kiln_store_i64(uint8_t *p, int64_t v) {
    kiln_store_u64(p, (uint64_t)v);
}

static inline void kiln_store_f32(uint8_t *p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    kiln_store_u32(p, bits);
}

static inline void kiln_store_f64(uint8_t *p, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    kiln_store_u64(p, bits);
}

static inline float kiln_load_f32(const uint8_t *p) {
    uint32_t bits = kiln_load_u32(p);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static inline double kiln_load_f64(const uint8_t *p) {
    uint64_t bits = kiln_load_u64(p);
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

#ifdef __cplusplus
}
#endif

#endif
