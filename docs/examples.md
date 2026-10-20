# Examples

The dynamic API and the generated API write the same bytes. Use the dynamic
API when the shape is chosen at runtime. Use a generated struct when a schema
already names the fields.

## Dynamic struct

`examples/main.c` encodes a point and decodes it. Run it with `make` and
`./build/example`.

```c
#include "kiln.h"

kiln_value fields[2];
fields[0].kind = KILN_I32;
fields[0].as.i64 = 1;
fields[1].kind = KILN_I32;
fields[1].as.i64 = 2;
```

The schema text is `struct{x:i32,y:i32,}`. The encoded buffer is eight bytes,
`x` then `y`, little-endian.

## Generated Point

`testdata/schema/point.json` has required `x` and `y` and an optional `label`.

```c
#include "point.h"

Point p;
uint8_t buf[64];
size_t n = 0;
memset(&p, 0, sizeof p);
p.x = 3;
p.y = -4;
p.label_present = 1;
p.label = "hi";
p.label_len = 2;
Point_encode(&p, buf, sizeof buf, &n);
```

`Point_encoded_size` reports `n`. `Point_decode` rejects a trailing byte and a
label that is not UTF-8. `Point_free` releases the label.

## Message

`testdata/schema/message.json` nests `Meta` through `#/$defs/Meta`.
`Message_encode` writes the scalar fields and then the nested struct.
`Meta_decode` reads a standalone meta value. `Message_decode_into` reads a
message that is followed by more bytes in a larger buffer.

## Tensor view

A decoded `tensors` value points `data` at the input buffer. The caller keeps
that buffer alive for as long as the view is used. Re-encoding copies those
bytes into a new canonical directory. The input itself is not modified.
