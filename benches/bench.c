#define _POSIX_C_SOURCE 200809L
#include "kiln.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t nsec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t best_encode(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf, int rounds) {
    int i;
    uint64_t best = UINT64_MAX;
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

int main(void) {
    enum { N = 4096, T = 1024, ROUNDS = 50 };
    kiln_arena *arena = kiln_arena_new();
    kiln_schema *point = NULL;
    kiln_schema *list;
    kiln_schema tensors = {KILN_TENSORS, NULL, NULL, 0};
    kiln_value batch;
    kiln_value one;
    kiln_tensor tensor;
    kiln_buf buf;
    uint8_t payload[T * 4];
    int32_t raw[N * 2];
    size_t i;
    uint64_t enc;
    uint64_t dec;
    if (!arena || kiln_schema_parse(arena, "struct{x:i32,y:i32,}", &point)) {
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
    printf("struct_batch n=%d encode_ns=%llu decode_ns=%llu bytes=%zu\n", N,
        (unsigned long long)enc, (unsigned long long)dec, buf.len);
    for (i = 0; i < T; i++) {
        kiln_store_f32(payload + i * 4, (float)i);
    }
    kiln_tensor_clear(&tensor);
    tensor.dtype = 2;
    tensor.rank = 1;
    tensor.shape[0] = T;
    tensor.data = payload;
    tensor.data_len = sizeof payload;
    memset(&one, 0, sizeof one);
    one.kind = KILN_TENSORS;
    one.as.tensors.items = &tensor;
    one.as.tensors.count = 1;
    kiln_buf_reset(&buf);
    if (kiln_encode_buf(&tensors, &one, &buf) != KILN_OK) {
        return 1;
    }
    enc = best_encode(&tensors, &one, &buf, ROUNDS);
    dec = best_decode(&tensors, buf.data, buf.len, ROUNDS, sizeof payload);
    printf("tensor_f32 n=%d encode_ns=%llu decode_ns=%llu bytes=%zu\n", T,
        (unsigned long long)enc, (unsigned long long)dec, buf.len);
    kiln_buf_free(&buf);
    kiln_arena_free(arena);
    return enc == 0 || dec == 0;
}
