# Why Kiln

Kiln is a binary layout for records and tensors. In this C library the struct
is the schema. Field names are not written, tags are not written, and the
bytes of field `n + 1` start where field `n` ends. A reader that knows the
declaration order can walk the buffer without a side table.

That is a good fit for C. An `int32_t` is four little-endian bytes. A list of
`float` is a count and then the array. A tensor directory does not widen those
numbers into a different container: the numeric payload stays a slice of the
input buffer, which the caller can pass to a numeric routine as the array.

A hostile count is rejected before allocation. The decoder reads the count,
checks it against the cap, then checks `count * min_size(element)` against the
unread bytes. Only a count that passes those steps may allocate. An empty
remainder with a huge count returns `EOF` and does not reserve a slot per
element.

`gld-kilngen-c --schema FILE --out DIR` emits C structs for a JSON Schema
subset. Each struct has `encoded_size`, `encode`, and `decode`. Optional
properties are a `0`/`1` tag. This library is not the specification. It is one
implementation of the wire.

---

## The wire

Frozen values and the version-2 tensor directory are specified here, and only
here:

<https://leo-gan.github.io/gld-kiln/spec/>

Scalars are little-endian. Strings carry a byte length and UTF-8. Dict keys
sort by their UTF-8 bytes. A frame starts with `KLN0` and a schema hash. A
hash that does not match the destination type is `SCHEMA`. The directory magic
is `KLNT` and the version byte is 2. This page does not restate the byte
tables. The linked document is normative.
