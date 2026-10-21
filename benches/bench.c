#define _POSIX_C_SOURCE 200809L
#include "kiln.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "f32 bulk bench treats host floats as little-endian wire bytes"
#endif

static uint64_t nsec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t best_encode(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf, int rounds) {
    int i;
    uint64_t best = UINT64_MAX;
    if (kiln_encode_buf(schema, value, buf) != KILN_OK) {
        return 0;
    }
    for (i = 0; i < rounds; i++) {
        uint64_t t0 = nsec();
        if (kiln_encode_buf(schema, value, buf) != KILN_OK) {
            return 0;
        }
        uint64_t dt = nsec() - t0;
        if (dt < best) {
            best = dt;
        }
    }
    return best;
}

static uint64_t best_decode(const kiln_schema *schema, const uint8_t *data, size_t len, int rounds, size_t expect) {
    int i;
    uint64_t best = UINT64_MAX;
    kiln_doc doc;
    memset(&doc, 0, sizeof doc);
    for (i = 0; i < rounds; i++) {
        uint64_t t0;
        uint64_t dt;
        if (kiln_decode_reuse(&doc, schema, data, len, NULL) != KILN_OK) {
            kiln_arena_free(doc.arena);
            return 0;
        }
        t0 = nsec();
        if (kiln_decode_reuse(&doc, schema, data, len, NULL) != KILN_OK) {
            kiln_arena_free(doc.arena);
            return 0;
        }
        dt = nsec() - t0;
        if (schema->kind == KILN_LIST) {
            if (doc.root.as.list.count != expect) {
                kiln_arena_free(doc.arena);
                return 0;
            }
        } else if (doc.root.as.tensors.count != 1 || doc.root.as.tensors.items[0].data_len != expect) {
            kiln_arena_free(doc.arena);
            return 0;
        }
        if (dt < best) {
            best = dt;
        }
    }
    kiln_arena_free(doc.arena);
    return best;
}

/* Decode, then copy into a buffer allocated once by the caller. */
static uint64_t best_decode_f32(const kiln_schema *schema, const uint8_t *data, size_t len, float *dst,
    size_t n, int rounds, int tensor) {
    int i;
    uint64_t best = UINT64_MAX;
    kiln_doc doc;
    uint32_t bits;
    memset(&doc, 0, sizeof doc);
    if (kiln_decode_reuse(&doc, schema, data, len, NULL) != KILN_OK) {
        kiln_arena_free(doc.arena);
        return 0;
    }
    for (i = 0; i < rounds; i++) {
        uint64_t t0 = nsec();
        uint64_t dt;
        if (kiln_decode_reuse(&doc, schema, data, len, NULL) != KILN_OK) {
            kiln_arena_free(doc.arena);
            return 0;
        }
        if (tensor) {
            if (doc.root.as.tensors.count != 1 ||
                kiln_tensor_load(&doc.root.as.tensors.items[0], dst, n, 4) != KILN_OK) {
                kiln_arena_free(doc.arena);
                return 0;
            }
        } else if (kiln_list_load(&doc.root, dst, n, 4) != KILN_OK) {
            kiln_arena_free(doc.arena);
            return 0;
        }
        memcpy(&bits, dst, 4);
        dt = nsec() - t0;
        if (bits != 0x80000000u) {
            kiln_arena_free(doc.arena);
            return 0;
        }
        if (dt < best) {
            best = dt;
        }
    }
    kiln_arena_free(doc.arena);
    return best;
}

static int same_bits(const float *got, const float *expect, size_t n) {
    uint32_t a;
    uint32_t b;
    if (memcmp(got, expect, n * 4u) != 0) {
        return 0;
    }
    memcpy(&a, &got[0], 4);
    memcpy(&b, &got[1], 4);
    return a == 0x80000000u && b == 0x7fc00000u;
}

int main(void) {
    enum { N = 4096, F = 1048576, ROUNDS = 30 };
    kiln_arena *arena = kiln_arena_new();
    kiln_schema *point = NULL;
    kiln_schema *list;
    kiln_schema *f32;
    kiln_schema tensors = {KILN_TENSORS, NULL, NULL, 0};
    kiln_value batch;
    kiln_value one;
    kiln_tensor tensor;
    kiln_buf buf;
    int32_t raw[N * 2];
    float *src;
    float *dst;
    size_t i;
    uint64_t enc;
    uint64_t dec;
    int bad = 0;
    uint32_t neg0 = 0x80000000u;
    uint32_t qnan = 0x7fc00000u;
    src = malloc((size_t)F * sizeof(float));
    dst = malloc((size_t)F * sizeof(float));
    if (!arena || !src || !dst || kiln_schema_parse(arena, "struct{x:i32,y:i32,}", &point)) {
        free(src);
        free(dst);
        kiln_arena_free(arena);
        return 1;
    }
    list = kiln_schema_new(arena, KILN_LIST);
    list->elem = point;
    for (i = 0; i < N; i++) {
        raw[i * 2] = (int32_t)i;
        raw[i * 2 + 1] = (int32_t)(0 - (int)i);
    }
    memset(&batch, 0, sizeof batch);
    batch.kind = KILN_LIST;
    batch.as.list.count = N;
    batch.as.list.bulk = 1;
    batch.as.list.elem_size = 8;
    batch.as.list.raw = (const uint8_t *)raw;
    kiln_buf_init(&buf);
    if (kiln_encode_buf(list, &batch, &buf) != KILN_OK) {
        return 1;
    }
    enc = best_encode(list, &batch, &buf, ROUNDS);
    dec = best_decode(list, buf.data, buf.len, ROUNDS, N);
    if (enc == 0 || dec == 0) {
        bad = 1;
    }
    printf("struct_batch n=%d encode_ns=%llu decode_ns=%llu bytes=%zu\n", N, (unsigned long long)enc,
        (unsigned long long)dec, buf.len);

    for (i = 0; i < F; i++) {
        src[i] = (float)(i % 1000) * 0.25f;
    }
    memcpy(&src[0], &neg0, 4);
    memcpy(&src[1], &qnan, 4);
    f32 = kiln_schema_new(arena, KILN_F32);
    list = kiln_schema_new(arena, KILN_LIST);
    list->elem = f32;
    memset(&batch, 0, sizeof batch);
    batch.kind = KILN_LIST;
    batch.as.list.count = F;
    batch.as.list.bulk = 1;
    batch.as.list.elem_size = 4;
    batch.as.list.raw = (const uint8_t *)src;
    kiln_buf_reset(&buf);
    if (kiln_buf_reserve(&buf, 4u + (size_t)F * 4u) != KILN_OK) {
        return 1;
    }
    enc = best_encode(list, &batch, &buf, ROUNDS);
    dec = best_decode_f32(list, buf.data, buf.len, dst, F, ROUNDS, 0);
    if (enc == 0 || dec == 0 || !same_bits(dst, src, F) || kiln_load_u32(buf.data + 4) != neg0 ||
        kiln_load_u32(buf.data + 8) != qnan) {
        bad = 1;
    }
    printf("list_f32 n=%d encode_ns=%llu decode_ns=%llu bytes=%zu\n", F, (unsigned long long)enc,
        (unsigned long long)dec, buf.len);

    kiln_tensor_clear(&tensor);
    tensor.dtype = 2;
    tensor.rank = 1;
    tensor.shape[0] = F;
    tensor.data = (const uint8_t *)src;
    tensor.data_len = (size_t)F * 4u;
    memset(&one, 0, sizeof one);
    one.kind = KILN_TENSORS;
    one.as.tensors.items = &tensor;
    one.as.tensors.count = 1;
    kiln_buf_reset(&buf);
    if (kiln_buf_reserve(&buf, 192u + (size_t)F * 4u) != KILN_OK) {
        return 1;
    }
    enc = best_encode(&tensors, &one, &buf, ROUNDS);
    memset(dst, 0, (size_t)F * 4u);
    dec = best_decode_f32(&tensors, buf.data, buf.len, dst, F, ROUNDS, 1);
    if (enc == 0 || dec == 0 || !same_bits(dst, src, F) || buf.len < 200 || kiln_load_u32(buf.data + 192) != neg0 ||
        kiln_load_u32(buf.data + 196) != qnan) {
        bad = 1;
    }
    printf("tensor_f32 n=%d encode_ns=%llu decode_ns=%llu bytes=%zu\n", F, (unsigned long long)enc,
        (unsigned long long)dec, buf.len);
    kiln_buf_free(&buf);
    kiln_arena_free(arena);
    free(src);
    free(dst);
    return bad;
}
