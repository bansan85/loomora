#!/usr/bin/env python
"""Validate a Loomora probe line (contracts/probe-result.schema.json) and its greeting.

Usage:
  python tools/check_probe.py [FILE] [--input TEXT]   probe line from FILE or standard input
  python tools/check_probe.py --self-test             built-in good and bad lines

Checks, in order:
  1. the line is ASCII and parses as one JSON object;
  2. the object validates against probe-result.schema.json (needs the `jsonschema` package);
  3. message == "Bonjour " + input (the input is the record's own `input`, or --input if given,
     in which case the record's `input` must equal it too);
  4. greeting_matches agrees with that comparison.

The input may hold other lines (a log): only the lines starting with "{" are candidates and
exactly one is required. Exit status 0 when the line is valid, 1 otherwise, 2 on a usage or
setup error. Importable: check_probe_line(line, expected_input=None) -> list of error strings.
"""
import argparse
import json
import sys

import common

SCHEMA_PATH = common.CONTRACTS / "probe-result.schema.json"
PREFIX = u"Bonjour "

_validator = None


def _get_validator():
    """Build the jsonschema validator once; raises ImportError if the package is missing."""
    global _validator
    if _validator is None:
        import jsonschema  # noqa: E402 (optional dependency, checked lazily)
        schema = common.read_json(SCHEMA_PATH)
        cls = jsonschema.validators.validator_for(schema)
        cls.check_schema(schema)
        _validator = cls(schema)
    return _validator


def check_probe_record(record, expected_input=None):
    """Return the list of problems of a decoded probe record (empty list = valid)."""
    errors = []
    validator = _get_validator()
    for err in sorted(validator.iter_errors(record), key=lambda e: list(e.absolute_path)):
        where = ".".join(str(p) for p in err.absolute_path) or "<record>"
        errors.append("schema: %s: %s" % (where, err.message))
    if errors:
        return errors  # the fields below may be missing or mistyped

    inp = record["input"]
    if expected_input is not None and inp != expected_input:
        errors.append("input: record has %r, expected %r" % (inp, expected_input))
    want = PREFIX + (expected_input if expected_input is not None else inp)
    same = record["message"] == want
    if not same:
        errors.append("message: %r != 'Bonjour ' + input = %r" % (record["message"], want))
    if record["greeting_matches"] != same:
        errors.append("greeting_matches is %r but the message %s 'Bonjour ' + input"
                      % (record["greeting_matches"], "equals" if same else "differs from"))
    return errors


def check_probe_line(line, expected_input=None):
    """Return the list of problems of one probe line (empty list = valid)."""
    text = line.rstrip("\r\n")
    try:
        text.encode("ascii")
    except UnicodeEncodeError:
        return ["line: not ASCII (non-ASCII characters must be \\uXXXX escapes)"]
    try:
        record = json.loads(text)
    except ValueError as e:
        return ["line: not valid JSON: %s" % e]
    if not isinstance(record, dict):
        return ["line: not a JSON object"]
    return check_probe_record(record, expected_input)


def find_probe_line(text):
    """Pick the probe line out of a text that may hold other output; (line, error)."""
    candidates = [l for l in text.splitlines() if l.lstrip().startswith("{")]
    if len(candidates) != 1:
        return None, "expected exactly one line starting with '{', found %d" % len(candidates)
    return candidates[0].strip(), None


# ---- self-test ---------------------------------------------------------------------------
# GOOD_LINE was produced by the real backend (loomora_probe_record, native legacy UI) and is
# pinned here; BAD_LINES are hand-written and must all be rejected.
GOOD_LINE = (
    '{"schema":1,"platform_id":"linux-ubuntu-22.04-x86_64","ui_variant":"modern",'
    '"input":"H\\u00e9l\\u00e8ne","message":"Bonjour H\\u00e9l\\u00e8ne","greeting_matches":true,'
    '"core":"cpp","compiler":"gcc 13.3.0","displayed":true,"display_ms":35,'
    '"peak_rss_kb":15000,"eol_warning_shown":false,"disabled_features":[],'
    '"timestamp_utc":"2026-11-12T10:00:01Z"}'
)


def _variant(**changes):
    rec = json.loads(GOOD_LINE)
    for k, v in changes.items():
        if v is _DELETE:
            del rec[k]
        else:
            rec[k] = v
    return json.dumps(rec)


_DELETE = object()

BAD_LINES = [
    ("message is not 'Bonjour ' + input", _variant(message="Bonjour Marie", greeting_matches=False)),
    ("lying greeting_matches", _variant(message="Bonjour Marie")),
    ("greeting_matches false but message right", _variant(greeting_matches=False)),
    ("missing field", _variant(core=_DELETE)),
    ("extra field", _variant(extra=1)),
    ("bad ui_variant", _variant(ui_variant="classic")),
    ("bad core", _variant(core="rust")),
    ("negative display_ms", _variant(display_ms=-1)),
    ("bad timestamp", _variant(timestamp_utc="2026-11-12 10:00:01")),
    ("wrong schema version", _variant(schema=2)),
    ("non-ASCII character in the line", GOOD_LINE.replace("\\u00e9", u"é", 1)),
    ("not JSON", "Bonjour"),
    ("JSON but not an object", "[1, 2]"),
]


def self_test():
    failures = 0
    errs = check_probe_line(GOOD_LINE)
    if errs:
        failures += 1
        print("FAIL good line rejected: " + "; ".join(errs))
    else:
        print("ok   good line (from the real backend) accepted")
    errs = check_probe_line(GOOD_LINE, expected_input=u"Hélène")
    if errs:
        failures += 1
        print("FAIL good line rejected with its own --input: " + "; ".join(errs))
    if not check_probe_line(GOOD_LINE, expected_input=u"Marie"):
        failures += 1
        print("FAIL good line accepted although --input differs")
    for name, line in BAD_LINES:
        errs = check_probe_line(line)
        if errs:
            print("ok   rejected (%s): %s" % (name, errs[0]))
        else:
            failures += 1
            print("FAIL bad line accepted (%s)" % name)
    print("self-test %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


def main():
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Validate a Loomora probe line.")
    ap.add_argument("file", nargs="?", help="file holding the probe line (default: standard input)")
    ap.add_argument("--input", dest="expected_input",
                    help="text the UI was given; the record's input and message must match it")
    ap.add_argument("--self-test", action="store_true", help="run the built-in good and bad lines")
    args = ap.parse_args()

    try:
        _get_validator()
    except ImportError:
        print("check_probe: the 'jsonschema' package is missing; install it with "
              "'python -m pip install --user jsonschema' (tools/requirements.txt)", file=sys.stderr)
        return 2

    if args.self_test:
        return self_test()

    if args.file:
        with open(args.file, encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        text = sys.stdin.read()
    line, problem = find_probe_line(text)
    if problem:
        print("check_probe: " + problem, file=sys.stderr)
        return 2
    errors = check_probe_line(line, args.expected_input)
    if errors:
        for e in errors:
            print("INVALID " + e)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
