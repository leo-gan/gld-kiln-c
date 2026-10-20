# gld-kiln-c design

gld-kiln-c is a from-scratch Kiln wire v2 library for ISO C11.
The runtime never calls a Kiln library written in another language. The Python
file `testdata/reference/kiln_ref.py` may be used as a test oracle. It is not
a build or run dependency.

## Goals

The library encodes and decodes Frozen values and the version-2 tensor
directory. A struct in C is the schema: fields are positional, names are not
on the wire, and there is no per-field tag. A tensor payload is a view of the
input buffer, so numeric bytes stay the caller's array. A hostile list or
tensor count fails the length check before the decoder allocates element
storage.

A second program, `gld-kilngen-c`, reads a JSON Schema subset and emits C
structs with `encoded_size`, `encode`, `decode`, and `decode_into`. A `tensors`
field is not produced from that subset.

## Values

Scalars are little-endian. `f32` and `f64` keep their IEEE bits, including a
negative zero and a quiet NaN payload. Strings are a `u32` byte length plus
UTF-8. `opt` is a `0` or `1` tag. Dict keys sort by raw UTF-8 bytes on encode.
Lists of `u8` and `i8`, and other fixed-width lists, are a view of the input
when the element size is known up front. Variable lists allocate one array of
values only after `count * min_size(element)` fits in the unread bytes.

The default cap is 16,777,216. It bounds counts and the length of one string
or byte run. A caller can raise it. A trusted reader skips UTF-8 checks and
quantization shape checks. It still rejects a length past the buffer.

## Tensor directory

Decode follows the version-2 check order. The error names are `FRAME`,
`LIMIT`, `EOF`, `TYPE`, `UTF8`, `TRAILING`, and `SCHEMA`. Descriptors are not
allocated until the directory and the payload block fit. Payload bytes are
pointers into the input. Encode writes one canonical block: sorted name heap,
64-byte payload placement, and a single copy of each numeric payload.

## Frame

The frame is `KLN0`, four zero bytes, and the FNV-1a 64 schema hash. The hash
mixes each token and then a trailing `0x00` step. A tensor field mixes the
ASCII bytes `tensors`. A mismatched hash is `SCHEMA`.

## Code generation

`gld-kilngen-c --schema FILE --out DIR` reads one JSON Schema document.
`boolean`, integer formats `int8` through `int64` and `uint8` through `uint64`,
`number` (`float` or the default `double`), `string`, `string` with format
`binary`, `array`, `object`, and `$ref` of `#/$defs/Name` are accepted. A
property left out of `required` is `opt`. If `required` is omitted, every
property is present. An object whose only value schema is
`additionalProperties` is `dict<str, V>`.

## Testing

`testdata/kiln-v2-cases.json` holds 46 accept and reject cases. Accept cases
decode, compare the recorded value, and re-encode to the same bytes. Reject
cases return the named error. Cases that are hostile counts also assert that
the decoder's allocation counter stays at zero. A separate test feeds bytes
from `kiln_ref.py` through the C codec, including negative zero and a quiet
NaN payload.
