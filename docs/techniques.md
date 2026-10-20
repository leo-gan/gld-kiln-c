# Techniques

The hot path is a cursor, a few width checks, and `memcpy`. Nothing in that
path calls a Kiln library written in another language.

## Check, then allocate

A list or dict count is rejected before element storage exists. The order is
the unread count word, the cap, the product `count * min_size(element)`, and
only then an allocation. A tensor count above the cap, or a directory that
does not fit, returns before a descriptor array exists. The test counter
`kiln_debug_alloc_total` stays at zero on those paths.

Fixed-width lists do not allocate elements at all. The value holds a pointer
into the input and a byte length. Encode writes the count and copies that
slice once.

## Tensor payloads

Decode of a directory borrows each payload. The pointer is `payload_base +
byte_offset` inside the caller's buffer. Encode computes canonical offsets in
one pass, zeroes the header, the name heap, and the alignment gaps, and
`memcpy`s each payload. It does not walk the numeric bytes one at a time.

`kiln_decode_reuse` keeps the small descriptor arena across calls so a loop
does not malloc a document on every tensor.

## Schema hash

The frame hash is FNV-1a 64. Each token is mixed a byte at a time, and each
token ends with the extra multiply that a `0x00` byte would cause. Struct
fields mix the name, a colon, the field type, and a comma. A tensor field
mixes the ASCII bytes `tensors`. The hash does not include tensor dtypes or
shapes. Those are payload.

## Generated code

Generated `decode` uses the same cap and the same minimum-size check as the
dynamic reader. A short list returns `EOF` before `calloc`. Strings are copied
so the caller can free the input buffer. Tensor fields are not part of the
JSON Schema subset, so generated structs do not borrow a directory.
