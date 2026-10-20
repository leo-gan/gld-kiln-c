#!/usr/bin/env python3
"""Round-trip Frozen bytes from testdata/reference/kiln_ref.py through the C codec."""

import importlib.util
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "build" / "kiln-tests"


def load_ref():
    path = ROOT / "testdata" / "reference" / "kiln_ref.py"
    spec = importlib.util.spec_from_file_location("kiln_ref", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(mode, schema, raw):
    proc = subprocess.run(
        [str(TOOL), mode, schema, raw.hex()],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    return proc


def expect_round(ref, schema_py, value, schema_text, framed=False):
    raw = ref.encode_framed(schema_py, value) if framed else ref.encode(schema_py, value)
    mode = "roundtrip-frame" if framed else "roundtrip"
    proc = run(mode, schema_text, raw)
    if proc.returncode != 0:
        sys.exit(f"{schema_text} framed={framed}: {proc.stderr}")
    lines = proc.stdout.splitlines()
    if not lines or lines[0] != raw.hex():
        sys.exit(f"bytes differ for {schema_text}\n{lines[:1]}\n{raw.hex()}")
    return proc.stdout


def main():
    ref = load_ref()
    point = "struct{x:i32,y:i32,}"
    message = (
        "struct{f_bool:bool,f_int32:i32,f_int64:i64,f_float64:f64,"
        "f_string:str,f_bool_2:bool,f_int32_2:i32,f_string_2:str,}"
    )
    expect_round(ref, ref.POINT, {"x": 3, "y": -4}, point)
    expect_round(ref, ref.POINT, {"x": 3, "y": -4}, point, framed=True)
    expect_round(
        ref,
        ref.MESSAGE,
        {
            "f_bool": True,
            "f_int32": -5,
            "f_int64": 99,
            "f_float64": 1.5,
            "f_string": "héllo",
            "f_bool_2": False,
            "f_int32_2": 0,
            "f_string_2": "",
        },
        message,
        framed=True,
    )
    expect_round(ref, ("list", ("i32",)), [1, -2, 3], "list<i32>")
    expect_round(ref, ("list", ("u8",)), [0, 1, 255], "list<u8>")
    expect_round(ref, ("list", ("i8",)), [0, -1, 127], "list<i8>")
    expect_round(ref, ("list", ("f32",)), [1.0, -2.0], "list<f32>")
    expect_round(ref, ("list", ("i32",)), [], "list<i32>")
    expect_round(ref, ("opt", ("i32",)), None, "opt<i32>")
    expect_round(ref, ("opt", ("i32",)), 9, "opt<i32>")
    expect_round(ref, ("opt", ("str",)), None, "opt<str>")
    expect_round(ref, ("opt", ("str",)), "zz", "opt<str>")
    expect_round(ref, ("dict", ("i32",)), {"b": 1, "a": 2}, "dict<str,i32>")
    expect_round(ref, ("dict", ("str",)), {}, "dict<str,str>")
    expect_round(ref, ("str",), "", "str")
    expect_round(ref, ("str",), "héllo", "str")
    expect_round(ref, ("bytes",), b"\x00\xff", "bytes")
    expect_round(ref, ("struct", []), {}, "struct{}")
    expect_round(ref, ("i64",), -99, "i64", framed=True)
    expect_round(ref, ("int",), 42, "int", framed=True)
    expect_round(ref, ("uint",), 2**64 - 1, "uint")
    text = expect_round(ref, ("f32",), -0.0, "f32", framed=True)
    if "F32 80000000" not in text:
        sys.exit("negative zero bits were not preserved")
    nan = struct.unpack("<d", struct.pack("<Q", 0x7FF8000000000001))[0]
    text = expect_round(ref, ("f64",), nan, "f64", framed=True)
    if f"{0x7FF8000000000001:016x}" not in text.lower():
        sys.exit("quiet NaN payload was not preserved\n" + text)
    raw = ref.encode_framed(ref.POINT, {"x": 1, "y": 2})
    proc = run("roundtrip-frame", "struct{x:i32,}", raw)
    if proc.returncode == 0 or "SCHEMA" not in proc.stderr:
        sys.exit(f"expected SCHEMA, got {proc.returncode} {proc.stderr}")
    print("oracle round trip passed")


if __name__ == "__main__":
    main()
