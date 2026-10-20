# gld-kiln-c

A from-scratch Kiln library for C. The runtime and the code generator are C.
They do not wrap, link, or vendor a Kiln library written in another language.

Python `testdata/reference/kiln_ref.py` is a **test oracle** only. It is not
required to encode or decode at runtime.

This repository is a standalone library. It is not part of any other project.

Documentation: [Why Kiln](https://leo-gan.github.io/gld-kiln-c/why-kiln/),
[Instructions](https://leo-gan.github.io/gld-kiln-c/instructions/),
[Examples](https://leo-gan.github.io/gld-kiln-c/examples/),
[Techniques](https://leo-gan.github.io/gld-kiln-c/techniques/),
[Test data](https://leo-gan.github.io/gld-kiln-c/test-data/).

## Install

GCC, ISO C11. The header to import is `kiln.h`. The static library is
`libkiln`. The generator binary is `gld-kilngen-c`. The version is the
contents of `VERSION`.

```bash
git clone https://github.com/leo-gan/gld-kiln-c.git
cd gld-kiln-c
make
```

Link a program with `-Iinclude -Lbuild -lkiln` and `#include "kiln.h"`.

```bash
bash scripts/run-tests.sh
```

## What it reads and writes

| Surface | Behavior |
| --- | --- |
| Frozen values | Little-endian scalars, strings, lists, optionals, dicts, and structs. |
| Tensor directory | Version-2 block. Payload bytes are a view of the input buffer. |
| Frame | Sixteen-byte `KLN0` prefix plus one Frozen value. A bad hash is `SCHEMA`. |
| JSON Schema | A subset that `gld-kilngen-c` turns into C structs with encode, decode, and encoded size. |

`list<u8>` and `list<i8>` are a count plus raw bytes. Other lists are a count
plus concatenated elements. Dict keys are sorted by UTF-8 bytes. A struct is
its fields in declaration order, with no names and no padding on the wire.

## Layout

`include/kiln.h` is the public header. `src/` is the codec, the JSON reader,
and `gld-kilngen-c`. `tests/generated` holds the C emitted from
`testdata/schema`.

Requires **GCC** and **C11**. This tree is developed with GCC 11.4.

## License

MIT. Copyright (c) 2026 Leonid Ganeline.
