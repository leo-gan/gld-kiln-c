#ifndef KILN_H
#define KILN_H

/* From-scratch Kiln wire v2 for C. ISO C11. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_VERSION "0.2.1"
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
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
#else
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
#endif
}

static inline uint32_t kiln_load_u32(const uint8_t *p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
#else
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
#endif
}

static inline uint64_t kiln_load_u64(const uint8_t *p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
#else
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
#endif
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
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
#else
    memcpy(p, &v, 2);
#endif
}

static inline void kiln_store_u32(uint8_t *p, uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
#else
    memcpy(p, &v, 4);
#endif
}

static inline void kiln_store_u64(uint8_t *p, uint64_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
#else
    memcpy(p, &v, 8);
#endif
}

static inline void kiln_store_i32(uint8_t *p, int32_t v) {
    kiln_store_u32(p, (uint32_t)v);
}

static inline void kiln_store_i64(uint8_t *p, int64_t v) {
    kiln_store_u64(p, (uint64_t)v);
}

static inline void kiln_store_f32(uint8_t *p, float v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint32_t bits;
    memcpy(&bits, &v, 4);
    kiln_store_u32(p, bits);
#else
    /* Little-endian host: the float's bits are already the wire image. */
    memcpy(p, &v, 4);
#endif
}

static inline void kiln_store_f64(uint8_t *p, double v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t bits;
    memcpy(&bits, &v, 8);
    kiln_store_u64(p, bits);
#else
    memcpy(p, &v, 8);
#endif
}

static inline float kiln_load_f32(const uint8_t *p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint32_t bits = kiln_load_u32(p);
    float v;
    memcpy(&v, &bits, 4);
    return v;
#else
    float v;
    memcpy(&v, p, 4);
    return v;
#endif
}

static inline double kiln_load_f64(const uint8_t *p) {
    double v;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t bits = kiln_load_u64(p);
    memcpy(&v, &bits, 8);
#else
    memcpy(&v, p, 8);
#endif
    return v;
}

/* Copy `count` scalars of `width` 1, 2, 4, or 8. On a little-endian host this
   is one memcpy of count * width. A 4-byte value is not widened into 8 bytes.
   Byte swap on big-endian is symmetric, so the same call converts host to
   little-endian wire and wire back to host. The ranges must not overlap. */
kiln_err kiln_bulk_copy(void *dst, const void *src, size_t count, size_t width);

/* Copy a bulk fixed-width list into host scalars. `width` must equal elem_size. */
kiln_err kiln_list_load(const kiln_value *list, void *dst, size_t count, size_t width);

/* Copy one tensor payload into host scalars. `count * width` must equal data_len. */
kiln_err kiln_tensor_load(const kiln_tensor *tensor, void *dst, size_t count, size_t width);

/* Fast ASCII-first inline UTF-8 validator; falls back to kiln_utf8_ok at first non-ASCII byte. */
static inline int kiln_utf8_fast(const uint8_t *s, size_t n) {
    size_t i = 0;
#if defined(__SSE2__)
    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
        if (_mm_movemask_epi8(chunk) != 0) {
            return kiln_utf8_ok(s + i, n - i);
        }
        i += 16;
    }
#endif
    while (i + 8 <= n) {
        uint64_t word;
        memcpy(&word, s + i, 8);
        if ((word & 0x8080808080808080ULL) != 0) {
            return kiln_utf8_ok(s + i, n - i);
        }
        i += 8;
    }
    while (i < n) {
        if (s[i] > 0x7Fu) {
            return kiln_utf8_ok(s + i, n - i);
        }
        i++;
    }
    return 1;
}

/* Direct positional writer into a caller-owned byte buffer. */
typedef struct kiln_writer {
    uint8_t *buf;
    size_t pos;
    size_t cap;
    uint64_t limit_cap;
} kiln_writer;

static inline void kiln_writer_init(kiln_writer *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->pos = 0;
    w->cap = cap;
    w->limit_cap = KILN_CAP_DEFAULT;
}

static inline kiln_err kiln_writer_bool(kiln_writer *w, int v) {
    if (w->cap - w->pos < 1) return KILN_ERR_LIMIT;
    w->buf[w->pos++] = v ? 1u : 0u;
    return KILN_OK;
}

static inline kiln_err kiln_writer_u32(kiln_writer *w, uint32_t v) {
    if (w->cap - w->pos < 4) return KILN_ERR_LIMIT;
    kiln_store_u32(w->buf + w->pos, v);
    w->pos += 4;
    return KILN_OK;
}

static inline kiln_err kiln_writer_i32(kiln_writer *w, int32_t v) {
    return kiln_writer_u32(w, (uint32_t)v);
}

static inline kiln_err kiln_writer_u64(kiln_writer *w, uint64_t v) {
    if (w->cap - w->pos < 8) return KILN_ERR_LIMIT;
    kiln_store_u64(w->buf + w->pos, v);
    w->pos += 8;
    return KILN_OK;
}

static inline kiln_err kiln_writer_i64(kiln_writer *w, int64_t v) {
    return kiln_writer_u64(w, (uint64_t)v);
}

static inline kiln_err kiln_writer_f32(kiln_writer *w, float v) {
    if (w->cap - w->pos < 4) return KILN_ERR_LIMIT;
    kiln_store_f32(w->buf + w->pos, v);
    w->pos += 4;
    return KILN_OK;
}

static inline kiln_err kiln_writer_f64(kiln_writer *w, double v) {
    if (w->cap - w->pos < 8) return KILN_ERR_LIMIT;
    kiln_store_f64(w->buf + w->pos, v);
    w->pos += 8;
    return KILN_OK;
}

static inline kiln_err kiln_writer_count(kiln_writer *w, size_t n) {
    if ((uint64_t)n > w->limit_cap || n > 0xFFFFFFFFu) return KILN_ERR_LIMIT;
    return kiln_writer_u32(w, (uint32_t)n);
}

static inline kiln_err kiln_writer_str(kiln_writer *w, const char *s, size_t len) {
    if ((uint64_t)len > w->limit_cap || len > 0xFFFFFFFFu) return KILN_ERR_LIMIT;
    if (w->cap - w->pos < 4u + len) return KILN_ERR_LIMIT;
    kiln_store_u32(w->buf + w->pos, (uint32_t)len);
    w->pos += 4;
    if (len) {
        memcpy(w->buf + w->pos, s, len);
        w->pos += len;
    }
    return KILN_OK;
}

static inline kiln_err kiln_writer_cstr(kiln_writer *w, const char *s) {
    return kiln_writer_str(w, s ? s : "", s ? strlen(s) : 0u);
}

static inline kiln_err kiln_writer_pod(kiln_writer *w, const void *src, size_t count, size_t width) {
    size_t nbytes;
    if ((uint64_t)count > w->limit_cap || count > 0xFFFFFFFFu) return KILN_ERR_LIMIT;
    if (width != 1 && width != 2 && width != 4 && width != 8) return KILN_ERR_TYPE;
    if (count > SIZE_MAX / width) return KILN_ERR_LIMIT;
    nbytes = count * width;
    if (w->cap - w->pos < 4u + nbytes) return KILN_ERR_LIMIT;
    kiln_store_u32(w->buf + w->pos, (uint32_t)count);
    w->pos += 4;
    if (nbytes) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        kiln_err err = kiln_bulk_copy(w->buf + w->pos, src, count, width);
        if (err != KILN_OK) return err;
#else
        memcpy(w->buf + w->pos, src, nbytes);
#endif
        w->pos += nbytes;
    }
    return KILN_OK;
}

/* Direct checked positional reader over a byte buffer. */
typedef struct kiln_reader {
    const uint8_t *p;
    const uint8_t *end;
    uint64_t cap;
    int trusted;
} kiln_reader;

static inline void kiln_reader_init(kiln_reader *r, const uint8_t *buf, size_t len, const kiln_read_opts *opts) {
    r->p = buf;
    r->end = buf + len;
    r->cap = (opts && opts->cap) ? opts->cap : KILN_CAP_DEFAULT;
    r->trusted = opts ? opts->trusted : 0;
}

static inline size_t kiln_reader_left(const kiln_reader *r) {
    return (size_t)(r->end - r->p);
}

static inline kiln_err kiln_reader_bool(kiln_reader *r, int *out) {
    uint8_t v;
    if (kiln_reader_left(r) < 1) return KILN_ERR_EOF;
    v = *r->p++;
    if (v > 1u) return KILN_ERR_TYPE;
    *out = (int)v;
    return KILN_OK;
}

static inline kiln_err kiln_reader_u32(kiln_reader *r, uint32_t *out) {
    if (kiln_reader_left(r) < 4) return KILN_ERR_EOF;
    *out = kiln_load_u32(r->p);
    r->p += 4;
    return KILN_OK;
}

static inline kiln_err kiln_reader_i32(kiln_reader *r, int32_t *out) {
    uint32_t v;
    kiln_err err = kiln_reader_u32(r, &v);
    if (err != KILN_OK) return err;
    *out = (int32_t)v;
    return KILN_OK;
}

static inline kiln_err kiln_reader_u64(kiln_reader *r, uint64_t *out) {
    if (kiln_reader_left(r) < 8) return KILN_ERR_EOF;
    *out = kiln_load_u64(r->p);
    r->p += 8;
    return KILN_OK;
}

static inline kiln_err kiln_reader_i64(kiln_reader *r, int64_t *out) {
    uint64_t v;
    kiln_err err = kiln_reader_u64(r, &v);
    if (err != KILN_OK) return err;
    *out = (int64_t)v;
    return KILN_OK;
}

static inline kiln_err kiln_reader_f32(kiln_reader *r, float *out) {
    if (kiln_reader_left(r) < 4) return KILN_ERR_EOF;
    *out = kiln_load_f32(r->p);
    r->p += 4;
    return KILN_OK;
}

static inline kiln_err kiln_reader_f64(kiln_reader *r, double *out) {
    if (kiln_reader_left(r) < 8) return KILN_ERR_EOF;
    *out = kiln_load_f64(r->p);
    r->p += 8;
    return KILN_OK;
}

static inline kiln_err kiln_reader_count(kiln_reader *r, size_t each_min, uint32_t *out) {
    uint32_t n;
    kiln_err err = kiln_reader_u32(r, &n);
    if (err != KILN_OK) return err;
    if ((uint64_t)n > r->cap) return KILN_ERR_LIMIT;
    if (each_min != 0) {
        if ((uint64_t)n > UINT64_MAX / each_min) return KILN_ERR_LIMIT;
        if ((uint64_t)n * each_min > kiln_reader_left(r)) return KILN_ERR_EOF;
    }
    *out = n;
    return KILN_OK;
}

static inline kiln_err kiln_reader_str(kiln_reader *r, const uint8_t **ptr_out, size_t *len_out) {
    uint32_t n;
    kiln_err err = kiln_reader_count(r, 1, &n);
    if (err != KILN_OK) return err;
    if (!r->trusted && !kiln_utf8_fast(r->p, n)) return KILN_ERR_UTF8;
    *ptr_out = r->p;
    *len_out = n;
    r->p += n;
    return KILN_OK;
}

static inline kiln_err kiln_reader_cstr(kiln_reader *r, char *dst, size_t dst_cap) {
    const uint8_t *s;
    size_t n;
    kiln_err err = kiln_reader_str(r, &s, &n);
    if (err != KILN_OK) return err;
    if (n >= dst_cap) return KILN_ERR_LIMIT;
    if (n) memcpy(dst, s, n);
    dst[n] = '\0';
    return KILN_OK;
}

static inline kiln_err kiln_reader_pod(kiln_reader *r, void *dst, size_t max_count, size_t width, uint32_t *count_out) {
    uint32_t n;
    size_t nbytes;
    kiln_err err;
    if (width != 1 && width != 2 && width != 4 && width != 8) return KILN_ERR_TYPE;
    err = kiln_reader_count(r, width, &n);
    if (err != KILN_OK) return err;
    if (n > max_count) return KILN_ERR_LIMIT;
    nbytes = (size_t)n * width;
    if (nbytes) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        err = kiln_bulk_copy(dst, r->p, n, width);
        if (err != KILN_OK) return err;
#else
        memcpy(dst, r->p, nbytes);
#endif
        r->p += nbytes;
    }
    *count_out = n;
    return KILN_OK;
}

static inline kiln_err kiln_reader_finish(const kiln_reader *r) {
    return r->p == r->end ? KILN_OK : KILN_ERR_TRAILING;
}

#ifdef __cplusplus
}
#endif

#endif
