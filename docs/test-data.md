# Test data

The tests read files under `testdata/`. Nothing in that tree is required at
runtime.

## Wire cases

`testdata/kiln-v2-cases.json` has 46 accept and reject cases for Kiln wire v2.
The cap recorded in the file is 16777216. Each case has hex input, `expect`
of `accept` or `reject`, and an error name when it rejects.

| Expect | What a passing run means |
| --- | --- |
| `accept` | Decode the hex, match `decoded` when it is present, and re-encode the same bytes. |
| `reject` | The decoder returns the case's `error` name. |

Cases whose id is about a hostile count also require the allocation counter to
stay at zero. `bash scripts/run-tests.sh` compiles `build/kiln-tests` and runs
that program on the file.

## Schemas

| File | What it generates |
| --- | --- |
| `testdata/schema/point.json` | `Point`, with optional `label`. |
| `testdata/schema/message.json` | `Message` and nested `Meta`. |
| `testdata/schema/batch.json` | `Batch`, with `f32` and `i64` lists, bools, and binary strings. |

`bash scripts/check-generated.sh` writes those files again and diffs them
against `tests/generated`. A drift fails the check.

## Oracle

`testdata/reference/kiln_ref.py` encodes Frozen values for the tests. The C
codec decodes those bytes and encodes them again. The set includes Point,
Message, lists, optionals, dicts, framed values, a negative zero, and a quiet
NaN payload. The reference is not linked into the library.
