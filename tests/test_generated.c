#include "kiln.h"
#include "point.h"
#include "message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        fails++;
    }
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
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

static void test_point(void) {
    Point in;
    Point out;
    uint8_t buf[64];
    size_t n = 0;
    size_t len = 0;
    char *json;
    kiln_arena *arena;
    kiln_schema *schema = NULL;
    kiln_value label;
    kiln_value opt;
    kiln_value fields[3];
    kiln_value root;
    uint8_t *dyn = NULL;
    size_t dn = 0;
    kiln_doc *doc = NULL;
    uint8_t bad[16];
    memset(&in, 0, sizeof in);
    in.x = 3;
    in.y = -4;
    in.label_present = 1;
    in.label = "hi";
    in.label_len = 2;
    expect(Point_encode(&in, buf, sizeof buf, &n) == KILN_OK, "point encode");
    expect(Point_encoded_size(&in) == n, "point size");
    expect(Point_decode(&out, buf, n) == KILN_OK, "point decode");
    expect(out.x == 3 && out.y == -4 && out.label_present && out.label_len == 2, "point fields");
    expect(out.label && memcmp(out.label, "hi", 2) == 0, "point label");
    Point_free(&out);
    json = read_all("testdata/schema/point.json", &len);
    arena = kiln_arena_new();
    expect(json && arena && kiln_schema_from_json(arena, json, len, &schema) == KILN_OK, "point schema");
    memset(&label, 0, sizeof label);
    label.kind = KILN_STR;
    label.as.bytes.ptr = (const uint8_t *)"hi";
    label.as.bytes.len = 2;
    memset(&opt, 0, sizeof opt);
    opt.kind = KILN_OPT;
    opt.as.opt.present = 1;
    opt.as.opt.item = &label;
    memset(fields, 0, sizeof fields);
    fields[0].kind = KILN_I32;
    fields[0].as.i64 = 3;
    fields[1].kind = KILN_I32;
    fields[1].as.i64 = -4;
    fields[2] = opt;
    memset(&root, 0, sizeof root);
    root.kind = KILN_STRUCT;
    root.as.strukt.fields = fields;
    root.as.strukt.count = 3;
    expect(kiln_encode(schema, &root, &dyn, &dn) == KILN_OK, "dynamic point");
    expect(dn == n && memcmp(dyn, buf, n) == 0, "generated matches dynamic");
    expect(kiln_decode(schema, buf, n, NULL, &doc) == KILN_OK, "dynamic decode");
    expect(doc && doc->root.as.strukt.fields[0].as.i64 == 3, "dynamic x");
    expect(doc->root.as.strukt.fields[2].as.opt.present == 1, "dynamic label present");
    kiln_doc_free(doc);
    free(dyn);
    memset(&in, 0, sizeof in);
    in.x = 1;
    in.y = 2;
    expect(Point_encode(&in, buf, sizeof buf, &n) == KILN_OK, "absent encode");
    expect(n == 9 && buf[8] == 0, "absent tag");
    expect(Point_decode(&out, buf, n) == KILN_OK && !out.label_present, "absent decode");
    Point_free(&out);
    memcpy(bad, buf, n);
    bad[n] = 1;
    expect(Point_decode(&out, bad, n + 1) == KILN_ERR_TRAILING, "trailing");
    buf[0] = 1;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;
    buf[4] = 2;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = 1;
    buf[9] = 1;
    buf[10] = 0;
    buf[11] = 0;
    buf[12] = 0;
    buf[13] = 0xff;
    expect(Point_decode(&out, buf, 14) == KILN_ERR_UTF8, "utf8");
    free(json);
    kiln_arena_free(arena);
}

static void test_message(void) {
    Message in;
    Message out;
    uint8_t buf[128];
    size_t n = 0;
    Meta meta_out;
    memset(&in, 0, sizeof in);
    in.f_bool = 1;
    in.f_int32 = -5;
    in.f_int64 = 99;
    in.f_float64 = 1.5;
    in.f_string = "ab";
    in.f_string_len = 2;
    in.f_bool_2 = 0;
    in.f_int32_2 = 8;
    in.f_string_2 = "";
    in.f_string_2_len = 0;
    in.meta.region = "us";
    in.meta.region_len = 2;
    in.meta.flag = 1;
    expect(Message_encode(&in, buf, sizeof buf, &n) == KILN_OK, "message encode");
    expect(Message_encoded_size(&in) == n, "message size");
    expect(Message_decode(&out, buf, n) == KILN_OK, "message decode");
    expect(out.f_bool == 1 && out.f_int32 == -5 && out.f_int64 == 99, "message scalars");
    expect(out.f_float64 == 1.5 && out.f_string_len == 2 && memcmp(out.f_string, "ab", 2) == 0, "message str");
    expect(out.f_bool_2 == 0 && out.f_int32_2 == 8 && out.f_string_2_len == 0, "message tail");
    expect(out.meta.flag == 1 && out.meta.region_len == 2 && memcmp(out.meta.region, "us", 2) == 0, "meta");
    Message_free(&out);
    memset(&meta_out, 0, sizeof meta_out);
    expect(Meta_decode(&meta_out, buf + (n - (4 + 2 + 1)), 4 + 2 + 1) == KILN_OK, "meta alone");
    expect(meta_out.flag == 1, "meta flag");
    Meta_free(&meta_out);
}

int main(void) {
    test_point();
    test_message();
    if (fails) {
        fprintf(stderr, "%d generated failure(s)\n", fails);
        return 1;
    }
    printf("generated structs passed\n");
    return 0;
}
