"""Slow Kiln Frozen reference codec.

The Mojo library is checked against bytes this module produces. The
reference does not import the Mojo package.
"""

from __future__ import annotations

import struct
from typing import Any


FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK = 0xFFFFFFFFFFFFFFFF


def _mix(h: int, data: bytes) -> int:
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & MASK
    # A trailing 0x00 byte is a multiply, because xor with zero changes nothing.
    h = (h * FNV_PRIME) & MASK
    return h


def type_hash(schema: Any) -> int:
    h = FNV_OFFSET
    return _hash_schema(h, schema)


def _hash_schema(h: int, schema: Any) -> int:
    kind = schema[0]
    if kind == "bool":
        return _mix(h, b"bool")
    if kind == "i8":
        return _mix(h, b"i8")
    if kind == "i16":
        return _mix(h, b"i16")
    if kind == "i32":
        return _mix(h, b"i32")
    if kind == "i64":
        return _mix(h, b"i64")
    if kind == "int":
        return _mix(h, b"int")
    if kind == "u8":
        return _mix(h, b"u8")
    if kind == "u16":
        return _mix(h, b"u16")
    if kind == "u32":
        return _mix(h, b"u32")
    if kind == "u64":
        return _mix(h, b"u64")
    if kind == "uint":
        return _mix(h, b"uint")
    if kind == "f32":
        return _mix(h, b"f32")
    if kind == "f64":
        return _mix(h, b"f64")
    if kind == "str":
        return _mix(h, b"str")
    if kind == "list":
        h = _mix(h, b"list<")
        h = _hash_schema(h, schema[1])
        return _mix(h, b">")
    if kind == "opt":
        h = _mix(h, b"opt<")
        h = _hash_schema(h, schema[1])
        return _mix(h, b">")
    if kind == "dict":
        h = _mix(h, b"dict<str,")
        h = _hash_schema(h, schema[1])
        return _mix(h, b">")
    if kind == "struct":
        h = _mix(h, b"struct{")
        for name, field in schema[1]:
            h = _mix(h, name.encode("utf-8"))
            h = _mix(h, b":")
            h = _hash_schema(h, field)
            h = _mix(h, b",")
        return _mix(h, b"}")
    raise ValueError("unknown schema " + str(kind))


def encode(schema: Any, value: Any) -> bytes:
    kind = schema[0]
    if kind == "bool":
        return bytes([1 if value else 0])
    if kind == "i8":
        return struct.pack("<b", value)
    if kind == "i16":
        return struct.pack("<h", value)
    if kind == "i32":
        return struct.pack("<i", value)
    if kind == "i64" or kind == "int":
        return struct.pack("<q", value)
    if kind == "u8":
        return struct.pack("<B", value)
    if kind == "u16":
        return struct.pack("<H", value)
    if kind == "u32":
        return struct.pack("<I", value)
    if kind == "u64" or kind == "uint":
        return struct.pack("<Q", value)
    if kind == "f32":
        return struct.pack("<f", value)
    if kind == "f64":
        return struct.pack("<d", value)
    if kind == "str":
        raw = value.encode("utf-8")
        return struct.pack("<I", len(raw)) + raw
    if kind == "bytes":
        raw = bytes(value)
        return struct.pack("<I", len(raw)) + raw
    if kind == "list":
        elem = schema[1]
        # list<u8> is a raw block, matching Mojo's bulk path for List[UInt8].
        if elem == ("u8",) or elem == ("i8",):
            if elem == ("u8",):
                raw = bytes(value)
            else:
                raw = struct.pack("<" + "b" * len(value), *value)
            return struct.pack("<I", len(value)) + raw
        parts = [encode(elem, item) for item in value]
        body = b"".join(parts)
        return struct.pack("<I", len(value)) + body
    if kind == "opt":
        if value is None:
            return bytes([0])
        return bytes([1]) + encode(schema[1], value)
    if kind == "dict":
        items = sorted(value.items(), key=lambda kv: kv[0].encode("utf-8"))
        parts = []
        for key, item in items:
            parts.append(encode(("str",), key))
            parts.append(encode(schema[1], item))
        return struct.pack("<I", len(items)) + b"".join(parts)
    if kind == "struct":
        parts = []
        for name, field in schema[1]:
            parts.append(encode(field, value[name]))
        return b"".join(parts)
    raise ValueError("unknown schema " + str(kind))


def encode_framed(schema: Any, value: Any) -> bytes:
    body = encode(schema, value)
    header = b"KLN0" + bytes([0, 0, 0, 0]) + struct.pack("<Q", type_hash(schema))
    return header + body


MESSAGE = (
    "struct",
    [
        ("f_bool", ("bool",)),
        ("f_int32", ("i32",)),
        ("f_int64", ("i64",)),
        ("f_float64", ("f64",)),
        ("f_string", ("str",)),
        ("f_bool_2", ("bool",)),
        ("f_int32_2", ("i32",)),
        ("f_string_2", ("str",)),
    ],
)


POINT = (
    "struct",
    [
        ("x", ("i32",)),
        ("y", ("i32",)),
    ],
)
