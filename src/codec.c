#include "kiln.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define KILN_HEADER 64u
#define KILN_DESC 128u
#define KILN_FLAG_NAME 0x01u
#define KILN_FLAG_LE 0x02u
#define KILN_OPAQUE_MIN 4096u
#define KILN_OPAQUE_MAX 32767u

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint64_t cap;
    int trusted;
    int depth;
    kiln_arena *arena;
} R;

static kiln_err add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return KILN_ERR_LIMIT;
    }
    *out = a + b;
    return KILN_OK;
}

static kiln_err mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return KILN_ERR_LIMIT;
    }
    *out = a * b;
    return KILN_OK;
}

static kiln_err align64_u(uint64_t n, uint64_t *out) {
    if (n > UINT64_MAX - 63) {
        return KILN_ERR_LIMIT;
    }
    *out = (n + 63u) & ~63u;
    return KILN_OK;
}

kiln_err kiln_bulk_copy(void *dst, const void *src, size_t count, size_t width) {
    size_t nbytes;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return KILN_ERR_TYPE;
    }
    if (count > SIZE_MAX / width) {
        return KILN_ERR_LIMIT;
    }
    nbytes = count * width;
    if (nbytes == 0) {
        return KILN_OK;
    }
    if (!dst || !src) {
        return KILN_ERR_TYPE;
    }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    {
        const uint8_t *s = src;
        uint8_t *d = dst;
        size_t i;
        size_t b;
        for (i = 0; i < count; i++) {
            for (b = 0; b < width; b++) {
                d[b] = s[width - 1u - b];
            }
            d += width;
            s += width;
        }
    }
#else
    memcpy(dst, src, nbytes);
#endif
    return KILN_OK;
}

kiln_err kiln_list_load(const kiln_value *list, void *dst, size_t count, size_t width) {
    if (!list || list->kind != KILN_LIST || !list->as.list.bulk) {
        return KILN_ERR_TYPE;
    }
    if (list->as.list.elem_size != width || list->as.list.count != count) {
        return KILN_ERR_TYPE;
    }
    if (count == 0) {
        return KILN_OK;
    }
    return kiln_bulk_copy(dst, list->as.list.raw, count, width);
}

kiln_err kiln_tensor_load(const kiln_tensor *tensor, void *dst, size_t count, size_t width) {
    size_t nbytes;
    if (!tensor || (width != 1 && width != 2 && width != 4 && width != 8)) {
        return KILN_ERR_TYPE;
    }
    if (count > SIZE_MAX / width) {
        return KILN_ERR_LIMIT;
    }
    nbytes = count * width;
    if (tensor->data_len != nbytes) {
        return KILN_ERR_TYPE;
    }
    if (nbytes == 0) {
        return KILN_OK;
    }
    return kiln_bulk_copy(dst, tensor->data, count, width);
}

static kiln_err element_count(uint8_t rank, const uint64_t *shape, uint64_t *out) {
    uint64_t count = 1;
    uint8_t i;
    kiln_err err;
    if (rank == 0) {
        *out = 1;
        return KILN_OK;
    }
    for (i = 0; i < rank; i++) {
        err = mul_u64(count, shape[i], &count);
        if (err) {
            return err;
        }
    }
    *out = count;
    return KILN_OK;
}

static int whole_width(uint16_t dtype) {
    static const int widths[20] = {0, 8, 4, 2, 2, 1, 1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 0, 0, 0, 1};
    if (dtype >= 1 && dtype <= 19) {
        return widths[dtype];
    }
    return 0;
}

static kiln_err expected_bytes(uint16_t dtype, uint64_t count, uint64_t *out) {
    int width = whole_width(dtype);
    if (width) {
        return mul_u64(count, (uint64_t)width, out);
    }
    if (dtype >= 16 && dtype <= 18) {
        if (count == UINT64_MAX) {
            return KILN_ERR_LIMIT;
        }
        *out = (count + 1) / 2;
        return KILN_OK;
    }
    return KILN_ERR_TYPE;
}

static kiln_err touch(R *r) {
    if (r->arena) {
        return KILN_OK;
    }
    r->arena = kiln_arena_new();
    if (!r->arena) {
        return KILN_ERR_LIMIT;
    }
    return KILN_OK;
}

static kiln_err alloc_n(R *r, size_t n, size_t align, void **out) {
    kiln_err err = touch(r);
    void *p;
    if (err) {
        return err;
    }
    p = kiln_arena_alloc(r->arena, n, align);
    if (!p) {
        return KILN_ERR_LIMIT;
    }
    *out = p;
    return KILN_OK;
}

static kiln_err check_tensor(const kiln_tensor *item) {
    uint8_t d;
    uint16_t dtype = item->dtype;
    uint64_t count;
    uint64_t expect;
    kiln_err err;
    if (item->rank > 8) {
        return KILN_ERR_TYPE;
    }
    for (d = item->rank; d < 8; d++) {
        if (item->shape[d] != 0) {
            return KILN_ERR_TYPE;
        }
    }
    if (dtype == 0 || (dtype >= 20 && dtype < KILN_OPAQUE_MIN) || dtype > KILN_OPAQUE_MAX) {
        return KILN_ERR_TYPE;
    }
    if (dtype >= KILN_OPAQUE_MIN) {
        if (item->layout_len == 0 || item->block_size == 0) {
            return KILN_ERR_TYPE;
        }
        if (item->data_len % item->block_size != 0) {
            return KILN_ERR_TYPE;
        }
        return KILN_OK;
    }
    if (item->layout_len != 0 || item->block_size != 0) {
        return KILN_ERR_TYPE;
    }
    err = element_count(item->rank, item->shape, &count);
    if (err) {
        return err;
    }
    err = expected_bytes(dtype, count, &expect);
    if (err) {
        return err;
    }
    if ((uint64_t)item->data_len != expect) {
        return KILN_ERR_TYPE;
    }
    return KILN_OK;
}

static kiln_err ceil_div_u64(uint64_t num, uint64_t den, uint64_t *out) {
    uint64_t q;
    if (den == 0) {
        return KILN_ERR_TYPE;
    }
    q = num / den;
    if (num % den) {
        if (q == UINT64_MAX) {
            return KILN_ERR_LIMIT;
        }
        q++;
    }
    *out = q;
    return KILN_OK;
}

static kiln_err check_quant(const kiln_tensor *items, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        const kiln_tensor *item = &items[i];
        uint16_t scheme = item->quant_scheme;
        uint32_t scale;
        uint32_t outer;
        uint64_t groups;
        uint64_t other_dims;
        uint64_t expect;
        uint64_t scale_count;
        uint64_t outer_count;
        uint8_t dim;
        kiln_err err;
        const kiln_tensor *other;
        const kiln_tensor *outer_item;
        if (scheme == 0) {
            if (item->group_size != 0 || item->quant_axis != 0xFFFF || item->scale_index != 0xFFFFFFFFu ||
                item->outer_index != 0xFFFFFFFFu) {
                return KILN_ERR_TYPE;
            }
            continue;
        }
        if (scheme != 1 && scheme != 2) {
            return KILN_ERR_TYPE;
        }
        if (item->rank == 0 || item->quant_axis >= item->rank || item->group_size == 0) {
            return KILN_ERR_TYPE;
        }
        scale = item->scale_index;
        if ((size_t)scale == i || (uint64_t)scale >= count) {
            return KILN_ERR_TYPE;
        }
        other = &items[scale];
        if (other->quant_scheme != 0) {
            return KILN_ERR_TYPE;
        }
        err = ceil_div_u64(item->shape[item->quant_axis], item->group_size, &groups);
        if (err) {
            return err;
        }
        other_dims = 1;
        for (dim = 0; dim < item->rank; dim++) {
            if (dim == item->quant_axis) {
                continue;
            }
            err = mul_u64(other_dims, item->shape[dim], &other_dims);
            if (err) {
                return err;
            }
        }
        err = mul_u64(groups, other_dims, &expect);
        if (err) {
            return err;
        }
        err = element_count(other->rank, other->shape, &scale_count);
        if (err) {
            return err;
        }
        if (scale_count != expect) {
            return KILN_ERR_TYPE;
        }
        if (scheme == 1) {
            if (item->outer_index != 0xFFFFFFFFu) {
                return KILN_ERR_TYPE;
            }
            continue;
        }
        outer = item->outer_index;
        if ((size_t)outer == i || outer == scale || (uint64_t)outer >= count) {
            return KILN_ERR_TYPE;
        }
        outer_item = &items[outer];
        err = element_count(outer_item->rank, outer_item->shape, &outer_count);
        if (err) {
            return err;
        }
        if (outer_item->quant_scheme != 0 || outer_item->dtype != 2 || outer_count != 1) {
            return KILN_ERR_TYPE;
        }
    }
    return KILN_OK;
}

static uint64_t desc_u64(const uint8_t *buf, size_t index, size_t rel) {
    return kiln_load_u64(buf + KILN_HEADER + index * KILN_DESC + rel);
}

static uint16_t desc_u16(const uint8_t *buf, size_t index, size_t rel) {
    return kiln_load_u16(buf + KILN_HEADER + index * KILN_DESC + rel);
}

static kiln_err dec_tensors(R *r, kiln_value *out) {
    const uint8_t *buf = r->data + r->pos;
    size_t len = r->len - r->pos;
    uint8_t flags;
    uint32_t count;
    uint32_t name_bytes;
    uint64_t payload_bytes;
    uint64_t directory_bytes;
    uint64_t expect_dir;
    uint64_t directory_end;
    uint64_t payload_base;
    uint64_t block_end;
    kiln_tensor *items = NULL;
    const uint8_t *heap;
    const uint8_t *payload;
    int names_flag;
    int has_name = 0;
    size_t index;
    size_t cursor;
    uint64_t end;
    kiln_err err;
    if (len < 4) {
        return KILN_ERR_EOF;
    }
    if (memcmp(buf, "KLNT", 4) != 0) {
        return KILN_ERR_FRAME;
    }
    if (len < KILN_HEADER) {
        return KILN_ERR_EOF;
    }
    if (buf[4] != 2) {
        return KILN_ERR_FRAME;
    }
    flags = buf[5];
    if ((flags & ~0x03u) != 0 || (flags & KILN_FLAG_LE) == 0) {
        return KILN_ERR_FRAME;
    }
    if (kiln_load_u16(buf + 6) != 0) {
        return KILN_ERR_FRAME;
    }
    for (index = 32; index < KILN_HEADER; index++) {
        if (buf[index] != 0) {
            return KILN_ERR_FRAME;
        }
    }
    count = kiln_load_u32(buf + 8);
    name_bytes = kiln_load_u32(buf + 12);
    payload_bytes = kiln_load_u64(buf + 16);
    directory_bytes = kiln_load_u64(buf + 24);
    if ((uint64_t)count > r->cap || (uint64_t)name_bytes > r->cap) {
        return KILN_ERR_LIMIT;
    }
    err = mul_u64(count, KILN_DESC, &expect_dir);
    if (err) {
        return err;
    }
    err = add_u64(expect_dir, name_bytes, &expect_dir);
    if (err) {
        return err;
    }
    if (directory_bytes != expect_dir) {
        return KILN_ERR_TYPE;
    }
    err = add_u64(KILN_HEADER, directory_bytes, &directory_end);
    if (err) {
        return err;
    }
    if (directory_end > len) {
        return KILN_ERR_EOF;
    }
    err = align64_u(directory_end, &payload_base);
    if (err) {
        return err;
    }
    err = add_u64(payload_base, payload_bytes, &block_end);
    if (err) {
        return err;
    }
    if (block_end > len) {
        return KILN_ERR_EOF;
    }
    for (index = (size_t)directory_end; index < (size_t)payload_base; index++) {
        if (buf[index] != 0) {
            return KILN_ERR_TYPE;
        }
    }
    if (count) {
        void *mem = NULL;
        err = alloc_n(r, (size_t)count * sizeof(kiln_tensor), sizeof(void *), &mem);
        if (err) {
            return err;
        }
        items = mem;
        memset(items, 0, (size_t)count * sizeof(kiln_tensor));
    }
    for (index = 0; index < count; index++) {
        const uint8_t *d = buf + KILN_HEADER + index * KILN_DESC;
        size_t tail;
        for (tail = 112; tail < KILN_DESC; tail++) {
            if (d[tail] != 0) {
                return KILN_ERR_FRAME;
            }
        }
        items[index].dtype = kiln_load_u16(d);
        items[index].rank = d[2];
        items[index].name_len = kiln_load_u16(d + 6);
        items[index].layout_len = kiln_load_u16(d + 10);
        items[index].block_size = kiln_load_u32(d + 12);
        items[index].quant_scheme = kiln_load_u16(d + 16);
        items[index].quant_axis = kiln_load_u16(d + 18);
        items[index].group_size = kiln_load_u32(d + 20);
        items[index].scale_index = kiln_load_u32(d + 24);
        items[index].outer_index = kiln_load_u32(d + 28);
        for (tail = 0; tail < 8; tail++) {
            items[index].shape[tail] = kiln_load_u64(d + 48 + 8 * tail);
        }
    }
    names_flag = (flags & KILN_FLAG_NAME) != 0;
    for (index = 0; index < count; index++) {
        if (items[index].name_len) {
            has_name = 1;
        }
    }
    if (names_flag != has_name) {
        return KILN_ERR_TYPE;
    }
    if (names_flag) {
        for (index = 0; index < count; index++) {
            if (items[index].name_len == 0) {
                return KILN_ERR_TYPE;
            }
        }
    }
    heap = buf + KILN_HEADER + (size_t)count * KILN_DESC;
    for (index = 0; index < count; index++) {
        uint16_t name_off = desc_u16(buf, index, 4);
        uint16_t name_len = desc_u16(buf, index, 6);
        uint16_t layout_off = desc_u16(buf, index, 8);
        uint16_t layout_len = desc_u16(buf, index, 10);
        uint32_t name_end;
        uint32_t layout_end;
        size_t seen;
        if (buf[KILN_HEADER + index * KILN_DESC + 3] != 0) {
            return KILN_ERR_FRAME;
        }
        if (name_len == 0) {
            if (name_off != 0xFFFF) {
                return KILN_ERR_TYPE;
            }
            items[index].name = NULL;
            items[index].name_len = 0;
        } else {
            if (name_off == 0xFFFF) {
                return KILN_ERR_TYPE;
            }
            name_end = (uint32_t)name_off + name_len;
            if (name_end > name_bytes) {
                return KILN_ERR_TYPE;
            }
            items[index].name = heap + name_off;
            items[index].name_len = name_len;
            if (!r->trusted && !kiln_utf8_ok(items[index].name, name_len)) {
                return KILN_ERR_UTF8;
            }
            for (seen = 0; seen < index; seen++) {
                if (items[seen].name_len == name_len &&
                    memcmp(items[seen].name, items[index].name, name_len) == 0) {
                    return KILN_ERR_TYPE;
                }
            }
        }
        if (layout_len == 0) {
            if (layout_off != 0xFFFF) {
                return KILN_ERR_TYPE;
            }
            items[index].layout = NULL;
            items[index].layout_len = 0;
        } else {
            layout_end = (uint32_t)layout_off + layout_len;
            if (layout_end > name_bytes) {
                return KILN_ERR_TYPE;
            }
            items[index].layout = heap + layout_off;
            items[index].layout_len = layout_len;
            if (!r->trusted && !kiln_utf8_ok(items[index].layout, layout_len)) {
                return KILN_ERR_UTF8;
            }
        }
    }
    cursor = 0;
    for (index = 0; index < count; index++) {
        if (!items[index].name_len) {
            continue;
        }
        if (cursor + items[index].name_len > name_bytes ||
            memcmp(heap + cursor, items[index].name, items[index].name_len) != 0) {
            return KILN_ERR_TYPE;
        }
        cursor += items[index].name_len;
    }
    for (index = 0; index < count; index++) {
        if (!items[index].layout_len) {
            continue;
        }
        if (cursor + items[index].layout_len > name_bytes ||
            memcmp(heap + cursor, items[index].layout, items[index].layout_len) != 0) {
            return KILN_ERR_TYPE;
        }
        cursor += items[index].layout_len;
    }
    if (cursor != name_bytes) {
        return KILN_ERR_TYPE;
    }
    for (index = 0; index < count; index++) {
        uint64_t off = desc_u64(buf, index, 32);
        uint64_t length = desc_u64(buf, index, 40);
        uint64_t item_end;
        err = add_u64(off, length, &item_end);
        if (err) {
            return err;
        }
        if (item_end > payload_bytes) {
            return KILN_ERR_TYPE;
        }
        items[index].data_len = (size_t)length;
        err = check_tensor(&items[index]);
        if (err) {
            return err;
        }
    }
    for (index = 0; index < count; index++) {
        size_t right;
        uint64_t a0 = desc_u64(buf, index, 32);
        uint64_t a1 = a0 + items[index].data_len;
        for (right = index + 1; right < count; right++) {
            uint64_t b0 = desc_u64(buf, right, 32);
            uint64_t b1 = b0 + items[right].data_len;
            if (a0 == a1 || b0 == b1) {
                continue;
            }
            if (a0 < b1 && b0 < a1) {
                return KILN_ERR_TYPE;
            }
        }
    }
    end = 0;
    for (index = 0; index < count; index++) {
        uint64_t off = desc_u64(buf, index, 32);
        uint64_t aligned;
        err = align64_u(end, &aligned);
        if (err) {
            return err;
        }
        if (off != aligned) {
            return KILN_ERR_TYPE;
        }
        end = off + items[index].data_len;
    }
    if (end != payload_bytes) {
        return KILN_ERR_TYPE;
    }
    payload = buf + (size_t)payload_base;
    for (index = 0; index < count; index++) {
        uint64_t off = desc_u64(buf, index, 32);
        const uint8_t *region;
        items[index].data = payload + (size_t)off;
        region = items[index].data;
        if (items[index].dtype >= 16 && items[index].dtype <= 18) {
            uint64_t elems;
            err = element_count(items[index].rank, items[index].shape, &elems);
            if (err) {
                return err;
            }
            if ((elems % 2) == 1 && items[index].data_len && (region[items[index].data_len - 1] >> 4) != 0) {
                return KILN_ERR_TYPE;
            }
        }
        if (items[index].dtype == 7) {
            size_t b;
            for (b = 0; b < items[index].data_len; b++) {
                if (region[b] > 1) {
                    return KILN_ERR_TYPE;
                }
            }
        }
    }
    end = 0;
    for (index = 0; index < count; index++) {
        uint64_t off = desc_u64(buf, index, 32);
        uint64_t gap;
        for (gap = end; gap < off; gap++) {
            if (payload[gap] != 0) {
                return KILN_ERR_TYPE;
            }
        }
        end = off + items[index].data_len;
    }
    if (!r->trusted) {
        err = check_quant(items, count);
        if (err) {
            return err;
        }
    }
    if (r->pos > SIZE_MAX - (size_t)block_end) {
        return KILN_ERR_LIMIT;
    }
    r->pos += (size_t)block_end;
    out->kind = KILN_TENSORS;
    out->as.tensors.names = names_flag;
    out->as.tensors.items = items;
    out->as.tensors.count = count;
    out->as.tensors.block_length = (size_t)block_end;
    return KILN_OK;
}

static kiln_err dec_value(R *r, const kiln_schema *schema, kiln_value *out);

static kiln_err dec_bytes_like(R *r, kiln_value *out, kiln_kind kind, int utf8) {
    uint32_t n;
    if (r->len - r->pos < 4) {
        return KILN_ERR_EOF;
    }
    n = kiln_load_u32(r->data + r->pos);
    if ((uint64_t)n > r->cap) {
        return KILN_ERR_LIMIT;
    }
    if ((uint64_t)n > r->len - r->pos - 4) {
        return KILN_ERR_EOF;
    }
    r->pos += 4;
    if (utf8 && !r->trusted && !kiln_utf8_ok(r->data + r->pos, n)) {
        return KILN_ERR_UTF8;
    }
    out->kind = kind;
    out->as.bytes.ptr = r->data + r->pos;
    out->as.bytes.len = n;
    r->pos += n;
    return KILN_OK;
}

static kiln_err dec_list(R *r, const kiln_schema *elem, kiln_value *out) {
    uint32_t count;
    size_t each;
    size_t fixed = 0;
    uint64_t need;
    int is_fixed;
    if (r->len - r->pos < 4) {
        return KILN_ERR_EOF;
    }
    count = kiln_load_u32(r->data + r->pos);
    if ((uint64_t)count > r->cap) {
        return KILN_ERR_LIMIT;
    }
    each = kiln_min_size(elem);
    if (each != 0 && (uint64_t)count > UINT64_MAX / each) {
        return KILN_ERR_LIMIT;
    }
    need = (uint64_t)count * (uint64_t)each;
    if (need > (uint64_t)(r->len - r->pos - 4)) {
        return KILN_ERR_EOF;
    }
    if (r->depth >= KILN_DEPTH_MAX) {
        return KILN_ERR_LIMIT;
    }
    r->pos += 4;
    r->depth++;
    memset(out, 0, sizeof(*out));
    out->kind = KILN_LIST;
    out->as.list.count = count;
    is_fixed = kiln_fixed_size(elem, &fixed);
    if (is_fixed) {
        size_t nbytes = 0;
        if (count && fixed > SIZE_MAX / count) {
            r->depth--;
            return KILN_ERR_LIMIT;
        }
        nbytes = (size_t)count * fixed;
        out->as.list.bulk = 1;
        out->as.list.elem_size = fixed;
        out->as.list.raw = r->data + r->pos;
        if (elem->kind == KILN_BOOL) {
            size_t b;
            for (b = 0; b < nbytes; b++) {
                if (r->data[r->pos + b] > 1) {
                    r->depth--;
                    return KILN_ERR_TYPE;
                }
            }
        }
        r->pos += nbytes;
        r->depth--;
        return KILN_OK;
    }
    if (count) {
        void *mem = NULL;
        kiln_value *items;
        uint32_t i;
        kiln_err err;
        err = alloc_n(r, (size_t)count * sizeof(kiln_value), sizeof(void *), &mem);
        if (err) {
            r->depth--;
            return err;
        }
        items = mem;
        memset(items, 0, (size_t)count * sizeof(kiln_value));
        for (i = 0; i < count; i++) {
            err = dec_value(r, elem, &items[i]);
            if (err) {
                r->depth--;
                return err;
            }
        }
        out->as.list.items = items;
    }
    r->depth--;
    return KILN_OK;
}

static kiln_err dec_dict(R *r, const kiln_schema *elem, kiln_value *out) {
    uint32_t count;
    size_t pair;
    uint64_t need;
    uint32_t i;
    kiln_value *keys = NULL;
    kiln_value *vals = NULL;
    kiln_err err;
    if (r->len - r->pos < 4) {
        return KILN_ERR_EOF;
    }
    count = kiln_load_u32(r->data + r->pos);
    if ((uint64_t)count > r->cap) {
        return KILN_ERR_LIMIT;
    }
    pair = 4 + kiln_min_size(elem);
    if (pair != 0 && (uint64_t)count > UINT64_MAX / pair) {
        return KILN_ERR_LIMIT;
    }
    need = (uint64_t)count * (uint64_t)pair;
    if (need > (uint64_t)(r->len - r->pos - 4)) {
        return KILN_ERR_EOF;
    }
    if (r->depth >= KILN_DEPTH_MAX) {
        return KILN_ERR_LIMIT;
    }
    r->pos += 4;
    r->depth++;
    memset(out, 0, sizeof(*out));
    out->kind = KILN_DICT;
    out->as.dict.count = count;
    if (count) {
        void *mem = NULL;
        err = alloc_n(r, (size_t)count * sizeof(kiln_value), sizeof(void *), &mem);
        if (err) {
            r->depth--;
            return err;
        }
        keys = mem;
        memset(keys, 0, (size_t)count * sizeof(kiln_value));
        err = alloc_n(r, (size_t)count * sizeof(kiln_value), sizeof(void *), &mem);
        if (err) {
            r->depth--;
            return err;
        }
        vals = mem;
        memset(vals, 0, (size_t)count * sizeof(kiln_value));
        for (i = 0; i < count; i++) {
            static const kiln_schema str_schema = {KILN_STR, NULL, NULL, 0};
            err = dec_value(r, &str_schema, &keys[i]);
            if (err) {
                r->depth--;
                return err;
            }
            err = dec_value(r, elem, &vals[i]);
            if (err) {
                r->depth--;
                return err;
            }
        }
    }
    out->as.dict.keys = keys;
    out->as.dict.vals = vals;
    r->depth--;
    return KILN_OK;
}

static kiln_err dec_value(R *r, const kiln_schema *schema, kiln_value *out) {
    size_t width;
    if (kiln_fixed_size(schema, &width) && schema->kind != KILN_STRUCT) {
        if (r->len - r->pos < width) {
            return KILN_ERR_EOF;
        }
        memset(out, 0, sizeof(*out));
        out->kind = schema->kind;
        switch (schema->kind) {
        case KILN_BOOL:
            if (r->data[r->pos] > 1) {
                return KILN_ERR_TYPE;
            }
            out->as.b = r->data[r->pos];
            break;
        case KILN_I8:
            out->as.i64 = kiln_load_i8(r->data + r->pos);
            break;
        case KILN_U8:
            out->as.u64 = r->data[r->pos];
            break;
        case KILN_I16:
            out->as.i64 = kiln_load_i16(r->data + r->pos);
            break;
        case KILN_U16:
            out->as.u64 = kiln_load_u16(r->data + r->pos);
            break;
        case KILN_I32:
            out->as.i64 = kiln_load_i32(r->data + r->pos);
            break;
        case KILN_U32:
            out->as.u64 = kiln_load_u32(r->data + r->pos);
            break;
        case KILN_I64:
        case KILN_INT:
            out->as.i64 = kiln_load_i64(r->data + r->pos);
            break;
        case KILN_U64:
        case KILN_UINT:
            out->as.u64 = kiln_load_u64(r->data + r->pos);
            break;
        case KILN_F32:
            out->as.f32_bits = kiln_load_u32(r->data + r->pos);
            break;
        case KILN_F64:
            out->as.f64_bits = kiln_load_u64(r->data + r->pos);
            break;
        default:
            return KILN_ERR_TYPE;
        }
        r->pos += width;
        return KILN_OK;
    }
    switch (schema->kind) {
    case KILN_STR:
        return dec_bytes_like(r, out, KILN_STR, 1);
    case KILN_BYTES:
        return dec_bytes_like(r, out, KILN_BYTES, 0);
    case KILN_LIST:
        return dec_list(r, schema->elem, out);
    case KILN_OPT: {
        uint8_t tag;
        kiln_err err;
        if (r->len - r->pos < 1) {
            return KILN_ERR_EOF;
        }
        tag = r->data[r->pos++];
        memset(out, 0, sizeof(*out));
        out->kind = KILN_OPT;
        if (tag == 0) {
            return KILN_OK;
        }
        if (tag != 1) {
            return KILN_ERR_TYPE;
        }
        if (r->depth >= KILN_DEPTH_MAX) {
            return KILN_ERR_LIMIT;
        }
        r->depth++;
        err = alloc_n(r, sizeof(kiln_value), sizeof(void *), (void **)&out->as.opt.item);
        if (err) {
            r->depth--;
            return err;
        }
        err = dec_value(r, schema->elem, out->as.opt.item);
        r->depth--;
        if (err) {
            return err;
        }
        out->as.opt.present = 1;
        return KILN_OK;
    }
    case KILN_DICT:
        return dec_dict(r, schema->elem, out);
    case KILN_STRUCT: {
        size_t i;
        kiln_err err;
        void *mem = NULL;
        if (r->depth >= KILN_DEPTH_MAX) {
            return KILN_ERR_LIMIT;
        }
        r->depth++;
        memset(out, 0, sizeof(*out));
        out->kind = KILN_STRUCT;
        out->as.strukt.count = schema->nfields;
        if (schema->nfields) {
            if (schema->nfields > SIZE_MAX / sizeof(kiln_value)) {
                r->depth--;
                return KILN_ERR_LIMIT;
            }
            err = alloc_n(r, schema->nfields * sizeof(kiln_value), sizeof(void *), &mem);
            if (err) {
                r->depth--;
                return err;
            }
            out->as.strukt.fields = mem;
            memset(out->as.strukt.fields, 0, schema->nfields * sizeof(kiln_value));
            for (i = 0; i < schema->nfields; i++) {
                err = dec_value(r, schema->fields[i].type, &out->as.strukt.fields[i]);
                if (err) {
                    r->depth--;
                    return err;
                }
            }
        }
        r->depth--;
        return KILN_OK;
    }
    case KILN_TENSORS:
        if (r->depth >= KILN_DEPTH_MAX) {
            return KILN_ERR_LIMIT;
        }
        r->depth++;
        {
            kiln_err err = dec_tensors(r, out);
            r->depth--;
            return err;
        }
    default:
        return KILN_ERR_TYPE;
    }
}

static kiln_err app(kiln_buf *buf, const void *bytes, size_t n) {
    kiln_err err = kiln_buf_reserve(buf, n);
    if (err) {
        return err;
    }
    if (n && bytes) {
        memcpy(buf->data + buf->len, bytes, n);
    }
    buf->len += n;
    return KILN_OK;
}

static kiln_err app_u32(kiln_buf *buf, uint32_t v) {
    uint8_t b[4];
    kiln_store_u32(b, v);
    return app(buf, b, 4);
}

static kiln_err enc_value(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf);

static int key_less(const kiln_value *keys, size_t i, size_t j) {
    size_t na = keys[i].as.bytes.len;
    size_t nb = keys[j].as.bytes.len;
    size_t n = na < nb ? na : nb;
    int cmp = 0;
    if (n) {
        cmp = memcmp(keys[i].as.bytes.ptr ? keys[i].as.bytes.ptr : (const uint8_t *)"",
            keys[j].as.bytes.ptr ? keys[j].as.bytes.ptr : (const uint8_t *)"", n);
    }
    if (cmp < 0) {
        return 1;
    }
    if (cmp > 0) {
        return 0;
    }
    return na < nb;
}

static void sort_keys(size_t *idx, size_t n, const kiln_value *keys) {
    size_t gap = n / 2;
    while (gap > 0) {
        size_t i;
        for (i = gap; i < n; i++) {
            size_t tmp = idx[i];
            size_t j = i;
            while (j >= gap && key_less(keys, tmp, idx[j - gap])) {
                idx[j] = idx[j - gap];
                j -= gap;
            }
            idx[j] = tmp;
        }
        gap /= 2;
    }
}

/* One unnamed tensor: 192-byte directory, then one payload copy. */
static kiln_err enc_one_tensor(const kiln_tensor *item, kiln_buf *buf) {
    size_t total;
    size_t start;
    size_t dim;
    uint8_t *block;
    uint8_t *d;
    kiln_err err;
    if (item->data_len > SIZE_MAX - 192u) {
        return KILN_ERR_LIMIT;
    }
    total = 192u + item->data_len;
    start = buf->len;
    err = kiln_buf_reserve(buf, total);
    if (err) {
        return err;
    }
    block = buf->data + start;
    memset(block, 0, 192u);
    memcpy(block, "KLNT", 4);
    block[4] = 2;
    block[5] = (uint8_t)KILN_FLAG_LE;
    kiln_store_u32(block + 8, 1);
    kiln_store_u32(block + 12, 0);
    kiln_store_u64(block + 16, item->data_len);
    kiln_store_u64(block + 24, KILN_DESC);
    d = block + KILN_HEADER;
    kiln_store_u16(d + 0, item->dtype);
    d[2] = item->rank;
    kiln_store_u16(d + 4, 0xFFFF);
    kiln_store_u16(d + 6, 0);
    kiln_store_u16(d + 8, 0xFFFF);
    kiln_store_u16(d + 10, 0);
    kiln_store_u32(d + 12, item->block_size);
    kiln_store_u16(d + 16, item->quant_scheme);
    kiln_store_u16(d + 18, item->quant_axis);
    kiln_store_u32(d + 20, item->group_size);
    kiln_store_u32(d + 24, item->scale_index);
    kiln_store_u32(d + 28, item->outer_index);
    kiln_store_u64(d + 32, 0);
    kiln_store_u64(d + 40, item->data_len);
    for (dim = 0; dim < 8; dim++) {
        kiln_store_u64(d + 48 + 8 * dim, item->shape[dim]);
    }
    if (item->data_len) {
        if (item->data) {
            memcpy(block + 192, item->data, item->data_len);
        } else {
            memset(block + 192, 0, item->data_len);
        }
    }
    buf->len = start + total;
    return KILN_OK;
}

static kiln_err enc_tensors(const kiln_value *value, kiln_buf *buf) {
    const kiln_tensor *items = value->as.tensors.items;
    size_t count = value->as.tensors.count;
    size_t name_bytes = 0;
    size_t index;
    int any_name = 0;
    uint64_t end = 0;
    uint64_t stack_offs[8];
    uint64_t *offs = stack_offs;
    uint64_t *heap_offs = NULL;
    uint64_t directory_bytes;
    uint64_t directory_end;
    uint64_t payload_base;
    uint64_t total;
    size_t start;
    size_t heap_off;
    uint8_t *block;
    uint8_t flags;
    kiln_err err;
    if (count > 0xFFFFFFFFu) {
        return KILN_ERR_LIMIT;
    }
    if (count && !items) {
        return KILN_ERR_TYPE;
    }
    if (count == 1 && !items[0].name_len && !items[0].layout_len) {
        return enc_one_tensor(items, buf);
    }
    for (index = 0; index < count; index++) {
        if ((items[index].name_len && !items[index].name) || (items[index].layout_len && !items[index].layout)) {
            return KILN_ERR_TYPE;
        }
        if (items[index].name_len) {
            any_name = 1;
            if (items[index].name_len > 0xFFFF) {
                return KILN_ERR_LIMIT;
            }
            name_bytes += items[index].name_len;
        }
        if (items[index].layout_len) {
            if (items[index].layout_len > 0xFFFF) {
                return KILN_ERR_LIMIT;
            }
            name_bytes += items[index].layout_len;
        }
    }
    if (name_bytes > 0xFFFFFFFFu) {
        return KILN_ERR_LIMIT;
    }
    err = mul_u64(count, KILN_DESC, &directory_bytes);
    if (err) {
        return err;
    }
    err = add_u64(directory_bytes, name_bytes, &directory_bytes);
    if (err) {
        return err;
    }
    err = add_u64(KILN_HEADER, directory_bytes, &directory_end);
    if (err) {
        return err;
    }
    err = align64_u(directory_end, &payload_base);
    if (err) {
        return err;
    }
    if (count > 8) {
        heap_offs = malloc(count * sizeof(*heap_offs));
        if (!heap_offs) {
            return KILN_ERR_LIMIT;
        }
        offs = heap_offs;
    }
    end = 0;
    for (index = 0; index < count; index++) {
        uint64_t off;
        err = align64_u(end, &off);
        if (err) {
            free(heap_offs);
            return err;
        }
        offs[index] = off;
        err = add_u64(off, items[index].data_len, &end);
        if (err) {
            free(heap_offs);
            return err;
        }
    }
    err = add_u64(payload_base, end, &total);
    if (err || total > SIZE_MAX) {
        free(heap_offs);
        return KILN_ERR_LIMIT;
    }
    start = buf->len;
    err = kiln_buf_reserve(buf, (size_t)total);
    if (err) {
        free(heap_offs);
        return err;
    }
    block = buf->data + start;
    /* Zero the directory and pad only. Payload bytes are copied or gap-filled. */
    memset(block, 0, (size_t)payload_base);
    memcpy(block, "KLNT", 4);
    block[4] = 2;
    flags = KILN_FLAG_LE;
    if (any_name) {
        flags = (uint8_t)(flags | KILN_FLAG_NAME);
    }
    block[5] = flags;
    kiln_store_u32(block + 8, (uint32_t)count);
    kiln_store_u32(block + 12, (uint32_t)name_bytes);
    kiln_store_u64(block + 16, end);
    kiln_store_u64(block + 24, directory_bytes);
    heap_off = 0;
    for (index = 0; index < count; index++) {
        uint8_t *d = block + KILN_HEADER + index * KILN_DESC;
        uint64_t off = offs[index];
        size_t dim;
        uint16_t name_off = 0xFFFF;
        uint16_t name_len = 0;
        if (items[index].name_len) {
            name_off = (uint16_t)heap_off;
            name_len = (uint16_t)items[index].name_len;
            memcpy(block + KILN_HEADER + count * KILN_DESC + heap_off, items[index].name, name_len);
            heap_off += name_len;
        }
        kiln_store_u16(d + 0, items[index].dtype);
        d[2] = items[index].rank;
        kiln_store_u16(d + 4, name_off);
        kiln_store_u16(d + 6, name_len);
        kiln_store_u16(d + 8, 0xFFFF);
        kiln_store_u16(d + 10, 0);
        kiln_store_u32(d + 12, items[index].block_size);
        kiln_store_u16(d + 16, items[index].quant_scheme);
        kiln_store_u16(d + 18, items[index].quant_axis);
        kiln_store_u32(d + 20, items[index].group_size);
        kiln_store_u32(d + 24, items[index].scale_index);
        kiln_store_u32(d + 28, items[index].outer_index);
        kiln_store_u64(d + 32, off);
        kiln_store_u64(d + 40, items[index].data_len);
        for (dim = 0; dim < 8; dim++) {
            kiln_store_u64(d + 48 + 8 * dim, items[index].shape[dim]);
        }
        if (index == 0) {
            if (off) {
                memset(block + (size_t)payload_base, 0, (size_t)off);
            }
        } else {
            uint64_t prev = offs[index - 1] + items[index - 1].data_len;
            if (off > prev) {
                memset(block + (size_t)payload_base + (size_t)prev, 0, (size_t)(off - prev));
            }
        }
        if (items[index].data_len) {
            uint8_t *slot = block + (size_t)payload_base + (size_t)off;
            if (items[index].data) {
                memcpy(slot, items[index].data, items[index].data_len);
            } else {
                memset(slot, 0, items[index].data_len);
            }
        }
    }
    free(heap_offs);
    heap_off = 0;
    for (index = 0; index < count; index++) {
        if (items[index].name_len) {
            heap_off += items[index].name_len;
        }
    }
    for (index = 0; index < count; index++) {
        uint8_t *d = block + KILN_HEADER + index * KILN_DESC;
        if (!items[index].layout_len) {
            continue;
        }
        kiln_store_u16(d + 8, (uint16_t)heap_off);
        kiln_store_u16(d + 10, (uint16_t)items[index].layout_len);
        memcpy(block + KILN_HEADER + count * KILN_DESC + heap_off, items[index].layout, items[index].layout_len);
        heap_off += items[index].layout_len;
    }
    buf->len = start + (size_t)total;
    return KILN_OK;
}

static kiln_err enc_fixed_scalar(const kiln_value *value, uint8_t *dst) {
    switch (value->kind) {
    case KILN_BOOL:
        dst[0] = value->as.b ? 1 : 0;
        return KILN_OK;
    case KILN_I8:
        dst[0] = (uint8_t)(int8_t)value->as.i64;
        return KILN_OK;
    case KILN_U8:
        dst[0] = (uint8_t)value->as.u64;
        return KILN_OK;
    case KILN_I16:
        kiln_store_u16(dst, (uint16_t)(int16_t)value->as.i64);
        return KILN_OK;
    case KILN_U16:
        kiln_store_u16(dst, (uint16_t)value->as.u64);
        return KILN_OK;
    case KILN_I32:
        kiln_store_i32(dst, (int32_t)value->as.i64);
        return KILN_OK;
    case KILN_U32:
        kiln_store_u32(dst, (uint32_t)value->as.u64);
        return KILN_OK;
    case KILN_I64:
    case KILN_INT:
        kiln_store_i64(dst, value->as.i64);
        return KILN_OK;
    case KILN_U64:
    case KILN_UINT:
        kiln_store_u64(dst, value->as.u64);
        return KILN_OK;
    case KILN_F32:
        kiln_store_u32(dst, value->as.f32_bits);
        return KILN_OK;
    case KILN_F64:
        kiln_store_u64(dst, value->as.f64_bits);
        return KILN_OK;
    default:
        return KILN_ERR_TYPE;
    }
}

static kiln_err enc_value(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf) {
    size_t width = 0;
    kiln_err err;
    if (schema->kind != KILN_LIST && schema->kind != KILN_OPT && schema->kind != KILN_DICT &&
        schema->kind != KILN_STRUCT && schema->kind != KILN_TENSORS && schema->kind != KILN_STR &&
        schema->kind != KILN_BYTES && kiln_fixed_size(schema, &width)) {
        uint8_t tmp[8];
        err = enc_fixed_scalar(value, tmp);
        if (err) {
            return err;
        }
        return app(buf, tmp, width);
    }
    switch (schema->kind) {
    case KILN_STR:
    case KILN_BYTES:
        if (value->as.bytes.len > 0xFFFFFFFFu) {
            return KILN_ERR_LIMIT;
        }
        err = app_u32(buf, (uint32_t)value->as.bytes.len);
        if (err) {
            return err;
        }
        return app(buf, value->as.bytes.ptr, value->as.bytes.len);
    case KILN_LIST: {
        size_t count = value->as.list.count;
        size_t i;
        if (count > 0xFFFFFFFFu) {
            return KILN_ERR_LIMIT;
        }
        err = app_u32(buf, (uint32_t)count);
        if (err) {
            return err;
        }
        if (value->as.list.bulk) {
            size_t fixed = 0;
            size_t nbytes;
            /* raw is already little-endian wire bytes, not a widened host slot. */
            if (!schema->elem || !kiln_fixed_size(schema->elem, &fixed) || fixed != value->as.list.elem_size) {
                return KILN_ERR_TYPE;
            }
            if (fixed != 0 && count > SIZE_MAX / fixed) {
                return KILN_ERR_LIMIT;
            }
            nbytes = count * fixed;
            if (nbytes && !value->as.list.raw) {
                return KILN_ERR_TYPE;
            }
            return app(buf, value->as.list.raw, nbytes);
        }
        /* Tagged scalars: one reserve, then direct stores. Structs stay recursive. */
        if (schema->elem && schema->elem->kind != KILN_STRUCT && kiln_fixed_size(schema->elem, &width)) {
            size_t nbytes = 0;
            uint8_t *dst;
            if (count && !value->as.list.items) {
                return KILN_ERR_TYPE;
            }
            if (width != 0 && count > SIZE_MAX / width) {
                return KILN_ERR_LIMIT;
            }
            nbytes = count * width;
            err = kiln_buf_reserve(buf, nbytes);
            if (err) {
                return err;
            }
            dst = buf->data + buf->len;
            for (i = 0; i < count; i++) {
                err = enc_fixed_scalar(&value->as.list.items[i], dst);
                if (err) {
                    return err;
                }
                dst += width;
            }
            buf->len += nbytes;
            return KILN_OK;
        }
        for (i = 0; i < count; i++) {
            err = enc_value(schema->elem, &value->as.list.items[i], buf);
            if (err) {
                return err;
            }
        }
        return KILN_OK;
    }
    case KILN_OPT:
        if (!value->as.opt.present) {
            return app(buf, "\0", 1);
        }
        err = app(buf, "\1", 1);
        if (err) {
            return err;
        }
        return enc_value(schema->elem, value->as.opt.item, buf);
    case KILN_DICT: {
        size_t count = value->as.dict.count;
        size_t *idx;
        size_t i;
        static const kiln_schema str_schema = {KILN_STR, NULL, NULL, 0};
        if (count > 0xFFFFFFFFu) {
            return KILN_ERR_LIMIT;
        }
        err = app_u32(buf, (uint32_t)count);
        if (err) {
            return err;
        }
        if (!count) {
            return KILN_OK;
        }
        idx = malloc(count * sizeof(*idx));
        if (!idx) {
            return KILN_ERR_LIMIT;
        }
        for (i = 0; i < count; i++) {
            idx[i] = i;
        }
        sort_keys(idx, count, value->as.dict.keys);
        for (i = 0; i < count; i++) {
            err = enc_value(&str_schema, &value->as.dict.keys[idx[i]], buf);
            if (!err) {
                err = enc_value(schema->elem, &value->as.dict.vals[idx[i]], buf);
            }
            if (err) {
                free(idx);
                return err;
            }
        }
        free(idx);
        return KILN_OK;
    }
    case KILN_STRUCT: {
        size_t i;
        if (value->as.strukt.count != schema->nfields) {
            return KILN_ERR_TYPE;
        }
        for (i = 0; i < schema->nfields; i++) {
            err = enc_value(schema->fields[i].type, &value->as.strukt.fields[i], buf);
            if (err) {
                return err;
            }
        }
        return KILN_OK;
    }
    case KILN_TENSORS:
        return enc_tensors(value, buf);
    default:
        return KILN_ERR_TYPE;
    }
}

kiln_err kiln_decode(const kiln_schema *schema, const uint8_t *buf, size_t len, const kiln_read_opts *opts,
    kiln_doc **out) {
    R r;
    kiln_value root;
    kiln_doc *doc;
    kiln_err err;
    int standalone = !opts || opts->standalone;
    memset(&r, 0, sizeof(r));
    r.data = buf;
    r.len = len;
    r.cap = (!opts || opts->cap == 0) ? KILN_CAP_DEFAULT : opts->cap;
    r.trusted = opts && opts->trusted;
    kiln_count_begin();
    err = dec_value(&r, schema, &root);
    if (!err && standalone && r.pos != r.len) {
        err = KILN_ERR_TRAILING;
    }
    if (err) {
        kiln_arena_free(r.arena);
        kiln_count_end();
        return err;
    }
    doc = malloc(sizeof(*doc));
    if (!doc) {
        kiln_arena_free(r.arena);
        kiln_count_end();
        return KILN_ERR_LIMIT;
    }
    doc->arena = r.arena;
    doc->root = root;
    *out = doc;
    kiln_count_end();
    return KILN_OK;
}

kiln_err kiln_decode_reuse(kiln_doc *doc, const kiln_schema *schema, const uint8_t *buf, size_t len,
    const kiln_read_opts *opts) {
    R r;
    kiln_value root;
    kiln_err err;
    int standalone = !opts || opts->standalone;
    if (!doc) {
        return KILN_ERR_TYPE;
    }
    kiln_arena_reset(doc->arena);
    memset(&r, 0, sizeof(r));
    r.data = buf;
    r.len = len;
    r.cap = (!opts || opts->cap == 0) ? KILN_CAP_DEFAULT : opts->cap;
    r.trusted = opts && opts->trusted;
    r.arena = doc->arena;
    kiln_count_begin();
    err = dec_value(&r, schema, &root);
    if (!err && standalone && r.pos != r.len) {
        err = KILN_ERR_TRAILING;
    }
    doc->arena = r.arena;
    if (err) {
        kiln_count_end();
        return err;
    }
    doc->root = root;
    kiln_count_end();
    return KILN_OK;
}

kiln_err kiln_decode_framed(const kiln_schema *schema, const uint8_t *buf, size_t len,
    const kiln_read_opts *opts, kiln_doc **out, uint64_t *hash_out) {
    uint64_t got;
    uint64_t expect;
    if (len < 4) {
        return KILN_ERR_EOF;
    }
    if (memcmp(buf, "KLN0", 4) != 0) {
        return KILN_ERR_FRAME;
    }
    if (len < 16) {
        return KILN_ERR_EOF;
    }
    if (buf[4] || buf[5] || buf[6] || buf[7]) {
        return KILN_ERR_FRAME;
    }
    got = kiln_load_u64(buf + 8);
    expect = kiln_type_hash(schema);
    if (got != expect) {
        return KILN_ERR_SCHEMA;
    }
    if (hash_out) {
        *hash_out = got;
    }
    return kiln_decode(schema, buf + 16, len - 16, opts, out);
}

void kiln_doc_free(kiln_doc *doc) {
    if (!doc) {
        return;
    }
    kiln_arena_free(doc->arena);
    free(doc);
}

kiln_err kiln_encode_buf(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf) {
    buf->len = 0;
    return enc_value(schema, value, buf);
}

kiln_err kiln_encode(const kiln_schema *schema, const kiln_value *value, uint8_t **out, size_t *len) {
    kiln_buf buf;
    kiln_err err;
    kiln_buf_init(&buf);
    err = kiln_encode_buf(schema, value, &buf);
    if (err) {
        kiln_buf_free(&buf);
        return err;
    }
    *out = buf.data;
    *len = buf.len;
    return KILN_OK;
}

kiln_err kiln_encode_framed_buf(const kiln_schema *schema, const kiln_value *value, kiln_buf *buf) {
    uint8_t hdr[16];
    kiln_err err;
    buf->len = 0;
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "KLN0", 4);
    kiln_store_u64(hdr + 8, kiln_type_hash(schema));
    err = app(buf, hdr, 16);
    if (err) {
        return err;
    }
    return enc_value(schema, value, buf);
}

kiln_err kiln_encode_framed(const kiln_schema *schema, const kiln_value *value, uint8_t **out, size_t *len) {
    kiln_buf buf;
    kiln_err err;
    kiln_buf_init(&buf);
    err = kiln_encode_framed_buf(schema, value, &buf);
    if (err) {
        kiln_buf_free(&buf);
        return err;
    }
    *out = buf.data;
    *len = buf.len;
    return KILN_OK;
}
