#!/usr/bin/env python
"""Generate vectors.inc (plain C++11 initializer lists) from core/tests/vectors.json.

Usage: python tools/gen_vectors.py [-o OUTPUT] [INPUT]

The output defaults to core/tests/vectors.inc (git-ignored); the CMake build writes it
into the build directory instead. The file is rewritten only when its content changes.
The vectors are checked for internal consistency first, so a typo in vectors.json fails
here and never reaches the tests.
"""
import argparse
import sys
from pathlib import Path

import common

MAX_INPUT = 4096
DEFAULT_OUTPUT = common.CORE / "tests" / "vectors.inc"
UI_VARIANTS = {1: "legacy", 2: "modern"}
CORES = {1: "cpp", 2: "cpp-wasm", 3: "cpp-js"}
FIELD_TYPES = ("int", "string", "bool", "int_or_null", "string_array")


def fail(message):
    sys.exit("gen_vectors: " + message)


def enc(value):
    """Decode an 'enc' of vectors.json into bytes."""
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, list):
        return b"".join(enc(v) for v in value)
    if isinstance(value, dict):
        if "repeat" in value:
            return enc(value["repeat"]) * int(value["count"])
        if "hex" in value:
            return bytes.fromhex(value["hex"])
    fail("unsupported text encoding: %r" % (value,))


def is_utf8(data):
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def cstr(data):
    """C++ string literal(s) for raw bytes: octal escapes (always 3 digits, so never
    swallowing the next character), split into adjacent literals of 48 bytes."""
    chunks = []
    for start in range(0, max(len(data), 1), 48):
        piece = []
        for b in data[start:start + 48]:
            ch = chr(b)
            if 0x20 <= b < 0x7F and ch not in '"\\?':
                piece.append(ch)
            else:
                piece.append("\\%03o" % b)
        chunks.append('"' + "".join(piece) + '"')
    return " ".join(chunks)


def sv(data):
    """A std::string initializer that keeps embedded NUL bytes."""
    return "std::string(%s, %d)" % (cstr(data), len(data))


def name_literal(value):
    return '"%s"' % value


def check_id(seen, case_id, section):
    if not case_id or case_id in seen:
        fail("%s: missing or duplicate id %r" % (section, case_id))
    seen.add(case_id)


def greet_rows(data):
    prefix = data["prefix"].encode("utf-8")
    rows, seen = [], set()
    for case in data["cases"]:
        check_id(seen, case.get("id"), "greet")
        cid = case["id"]
        capacity = int(case.get("capacity", data["default_capacity"]))
        null_input = case["input"] is None
        raw = b"" if null_input else enc(case["input"])
        length = int(case["input_len"]) if null_input else len(raw)
        if "output" in case:
            out = enc(case["output"])
            if null_input or out != prefix + raw:
                fail("greet/%s: output is not prefix + input" % cid)
            if len(raw) > MAX_INPUT or not is_utf8(raw) or len(out) > capacity:
                fail("greet/%s: a success case must be valid, within limits and fit" % cid)
            status = len(out)
        else:
            out = b""
            status = int(case["error"])
            if status == -1 and not (len(raw) > MAX_INPUT and is_utf8(raw)):
                fail("greet/%s: -1 needs a valid input over %d bytes" % (cid, MAX_INPUT))
            if status == -2 and not (len(raw) <= MAX_INPUT and is_utf8(raw)
                                     and capacity < len(prefix + raw)):
                fail("greet/%s: -2 needs a valid input and a capacity too small" % cid)
            if status == -3 and not (null_input or not is_utf8(raw)):
                fail("greet/%s: -3 needs a null pointer or invalid UTF-8" % cid)
            if status not in (-1, -2, -3):
                fail("greet/%s: unexpected error code %d" % (cid, status))
        rows.append("    {%s, %s, %s, %d, %d, %d, %s}," % (
            name_literal(cid), "true" if null_input else "false",
            sv(raw), length, capacity, status, sv(out)))
    return rows


def eol_rows(data):
    rows, seen = [], set()
    for case in data["cases"]:
        check_id(seen, case.get("id"), "eol")
        if case["eol"] not in (0, 1):
            fail("eol/%s: eol must be 0 or 1" % case["id"])
        rows.append("    {%s, %d, %d, %d, %d}," % (
            name_literal(case["id"]), case["family"], case["major"], case["minor"], case["eol"]))
    return rows


def probe_shape_rows(data):
    rows = []
    for field in data["shape"]:
        if field["type"] not in FIELD_TYPES:
            fail("probe/shape: unknown type %r" % field["type"])
        rows.append("    {%s, %s}," % (name_literal(field["name"]), name_literal(field["type"])))
    return rows




def boolean(value):
    return "true" if value else "false"


def probe_rows(data):
    rows, seen = [], set()
    for case in data["cases"]:
        check_id(seen, case.get("id"), "probe")
        a, e = case["args"], case["expect"]
        cid = case["id"]
        if UI_VARIANTS[a["ui_variant"]] != e["ui_variant"] or CORES[a["core_kind"]] != e["core"]:
            fail("probe/%s: expected ui_variant/core inconsistent with the arguments" % cid)
        if (e["peak_rss_kb"] is None) != (a["rss_kb"] < 0):
            fail("probe/%s: peak_rss_kb must be null exactly when rss_kb < 0" % cid)
        if [f for f in a["disabled_features"].split(",") if f] != e["disabled_features"]:
            fail("probe/%s: disabled_features inconsistent" % cid)
        for text in (a["input"], a["displayed_text"], e["input"], e["message"]):
            if not is_utf8(enc(text)):
                fail("probe/%s: texts must be valid UTF-8" % cid)
        values = [
            name_literal(cid),
            # call arguments
            sv(enc(a["platform_id"])), str(a["ui_variant"]), str(a["core_kind"]),
            sv(enc(a["input"])), sv(enc(a["displayed_text"])), str(a["displayed"]),
            "%dU" % a["display_ms"], "%dLL" % a["rss_kb"], str(a["eol_warning_shown"]),
            sv(enc(a["disabled_features"])), sv(enc(a["timestamp_utc"])),
            # expected decoded fields
            name_literal(e["ui_variant"]), name_literal(e["core"]),
            sv(enc(e["input"])), sv(enc(e["message"])), boolean(e["greeting_matches"]),
            boolean(e["displayed"]), "%dULL" % e["display_ms"],
            boolean(e["peak_rss_kb"] is None), "%dLL" % (e["peak_rss_kb"] or 0),
            boolean(e["eol_warning_shown"]),
            "{%s}" % ", ".join(sv(enc(f)) for f in e["disabled_features"]),
            sv(enc(e["platform_id"])), sv(enc(e["timestamp_utc"])),
        ]
        rows.append("    {" + ",\n     ".join(values) + "},")
    return rows


HEADER = """\
// Generated by tools/gen_vectors.py from core/tests/vectors.json. Do not edit.
// Plain C++11, shared by the Catch2 tests and by loomora_selftest.
#ifndef LOOMORA_VECTORS_INC
#define LOOMORA_VECTORS_INC

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vectors {

// loomora_greet: expected_status >= 0 is the byte length of expected_output.
struct GreetVector {
    const char *id;
    bool null_input; // pass a NULL input pointer (with input_len)
    std::string input;
    std::size_t input_len;
    std::size_t capacity;
    std::int32_t expected_status;
    std::string expected_output;
};

// loomora_is_eol
struct EolVector {
    const char *id;
    std::int32_t family;
    std::int32_t major;
    std::int32_t minor;
    std::int32_t expected;
};

// One field of the probe record: type is int, string, bool, int_or_null or string_array.
struct ProbeField {
    const char *name;
    const char *type;
};

// loomora_probe_record: call arguments, then the decoded content expected back.
struct ProbeVector {
    const char *id;
    std::string platform_id;
    std::int32_t ui_variant;
    std::int32_t core_kind;
    std::string input;
    std::string displayed_text;
    std::int32_t displayed;
    std::uint32_t display_ms;
    std::int64_t rss_kb;
    std::int32_t eol_warning_shown;
    std::string disabled_features;
    std::string timestamp_utc;
    const char *expect_ui_variant;
    const char *expect_core;
    std::string expect_input;
    std::string expect_message;
    bool expect_greeting_matches;
    bool expect_displayed;
    std::uint64_t expect_display_ms;
    bool expect_peak_rss_null;
    std::int64_t expect_peak_rss_kb;
    bool expect_eol_warning_shown;
    std::vector<std::string> expect_disabled_features;
    std::string expect_platform_id;
    std::string expect_timestamp_utc;
};

"""


def table(type_name, var, rows):
    return "static const %s %s[] = {\n%s\n};\nstatic const std::size_t %sCount = sizeof(%s) / sizeof(%s[0]);\n\n" % (
        type_name, var, "\n".join(rows), var, var, var)


def generate(data):
    greet = data["greet"]
    out = [HEADER]
    out.append("static const std::size_t kGreetMaxInput = %d;\n" % greet["max_input_bytes"])
    out.append("static const char *const kGreetPrefix = %s;\n\n" % cstr(greet["prefix"].encode("utf-8")))
    out.append(table("GreetVector", "kGreetVectors", greet_rows(greet)))
    out.append(table("EolVector", "kEolVectors", eol_rows(data["eol"])))
    out.append(table("ProbeField", "kProbeFields", probe_shape_rows(data["probe"])))
    out.append(table("ProbeVector", "kProbeVectors", probe_rows(data["probe"])))
    out.append("} // namespace vectors\n\n#endif\n")
    return "".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", nargs="?", default=str(common.VECTORS))
    parser.add_argument("-o", "--output", default=str(DEFAULT_OUTPUT))
    args = parser.parse_args()

    text = generate(common.read_json(args.input))
    output = Path(args.output)
    if output.exists() and output.read_text(encoding="utf-8") == text:
        return 0
    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
