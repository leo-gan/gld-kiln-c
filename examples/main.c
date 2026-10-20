#include "kiln.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Encode a positional Point and read it back. */
int main(void) {
    kiln_arena *arena = kiln_arena_new();
    kiln_schema *schema = NULL;
    kiln_value fields[2];
    kiln_value root;
    uint8_t *bytes = NULL;
    size_t n = 0;
    kiln_doc *doc = NULL;
    if (!arena || kiln_schema_parse(arena, "struct{x:i32,y:i32,}", &schema)) {
        fprintf(stderr, "schema\n");
        return 1;
    }
    memset(fields, 0, sizeof fields);
    fields[0].kind = KILN_I32;
    fields[0].as.i64 = 1;
    fields[1].kind = KILN_I32;
    fields[1].as.i64 = 2;
    memset(&root, 0, sizeof root);
    root.kind = KILN_STRUCT;
    root.as.strukt.fields = fields;
    root.as.strukt.count = 2;
    if (kiln_encode(schema, &root, &bytes, &n) != KILN_OK) {
        fprintf(stderr, "encode\n");
        return 1;
    }
    if (kiln_decode(schema, bytes, n, NULL, &doc) != KILN_OK) {
        fprintf(stderr, "decode\n");
        return 1;
    }
    printf("point bytes %zu x %lld y %lld\n", n,
        (long long)doc->root.as.strukt.fields[0].as.i64,
        (long long)doc->root.as.strukt.fields[1].as.i64);
    printf("generator: gld-kilngen-c --schema testdata/schema/point.json --out tests/generated\n");
    kiln_doc_free(doc);
    free(bytes);
    kiln_arena_free(arena);
    return 0;
}
