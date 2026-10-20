# Instructions

These steps assume a Linux machine and GCC. The header is `kiln.h`. The
command-line generator is `gld-kilngen-c`.

## Install

```bash
git clone https://github.com/leo-gan/gld-kiln-c.git
cd gld-kiln-c
make
```

`#include "kiln.h"` after adding `-Iinclude` to the compiler and `-Lbuild -lkiln`
to the link. The version string is `kiln_version()`, and the same text is in
the `VERSION` file.

## Build this repository

```bash
bash scripts/run-tests.sh
```

GCC 11.4 or a newer C11 compiler is enough. The flags are `-std=c11 -Wall
-Wextra -Werror`.

| Script | What it runs |
| --- | --- |
| `bash scripts/run-tests.sh` | The 46 wire cases, generated structs, and the Python oracle. |
| `bash scripts/generate.sh` | Writes `tests/generated` from `testdata/schema`. |
| `bash scripts/check-generated.sh` | Regenerates into a temp directory and diffs. |
| `bash scripts/run-microbench.sh` | Prints nanoseconds for a struct batch and one f32 tensor. |

## Decode and encode

```c
#include "kiln.h"

kiln_arena *arena = kiln_arena_new();
kiln_schema *schema = NULL;
kiln_schema_parse(arena, "struct{x:i32,y:i32,}", &schema);
```

`kiln_decode` reads one value. `kiln_encode` writes it. Pass `kiln_read_opts`
when the cap should be higher than 16777216, when UTF-8 and quant-shape checks
should be skipped (`trusted`), or when trailing bytes are allowed
(`standalone` set to 0).

## Generate C

```bash
./build/gld-kilngen-c --schema testdata/schema/point.json --out tests/generated
./build/gld-kilngen-c --schema testdata/schema/message.json --out tests/generated
```

The generator writes one `.h` and one `.c` per schema file. Compile them with
the Kiln header. Nested `$ref` structs are emitted in the same file, defined
before the structs that contain them.

## Document the package

```bash
pip install -r requirements-docs.txt
mkdocs build --strict
```
