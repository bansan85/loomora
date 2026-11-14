#!/usr/bin/env python
"""Check that no UI builds the greeting itself or holds application logic (FR-010, FR-029, T108).

Usage:
  python tools/check_ui_logic.py [--root DIR] [--verbose] [--selftest]

Searches every UI under ui/ (ui/<variant>/<platform>/, every file type: sources, resources, build
files, manifests) for the greeting prefix and for code that could build the greeting:

  * the literal "Bonjour" or the word "hello" (any case) in CODE, that is outside comments, and
    also when a literal is split ("Bon" "jour", C string concatenation). Those are violations:
    the greeting prefix lives only in core/src/greet.cpp (FR-009, FR-028).
  * a UI that has source files but never refers to the greeting call (loomora_greet, or greet as a whole word,
    in code, as through a bridge): it cannot obtain the greeting from the backend (FR-029).

Allowed, by decision of T108: the prefilled default "Coucou" (a UI may hold it, as a literal or as
a resource string), and the words "Bonjour" / "hello" in comments (documentation of the contract,
for example "Bonjour " + input in the comment of the buffer size). Comment mentions are counted and
listed with --verbose. Files that are tests or documentation (a directory named test, tests, docs,
doc or a *.md, *.txt-doc file) are skipped; CMakeLists.txt is checked like a source.

What it is: a text search, not a parser. Comments are removed with a small scanner for the C family
(// and /* */), for the hash family (CMake, Python, shell, properties) and for XML (<!-- -->);
string literals are only joined when they are adjacent. A greeting that is built from escapes or
computed characters would not be seen, which is why the run-time proof is tools/prefix_check.py
(the prefix is changed in the backend only and every UI follows).

Exit status: 0 no violation, 1 a violation, 2 a usage error. `--selftest` checks the checker on a
temporary tree (a clean UI, each kind of violation, each allowed form) and exits 0 when it behaves.
Python 3.8+, standard library only.
"""
import argparse
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

import common

C_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".rc", ".java", ".kt", ".kts",
         ".swift", ".m", ".mm", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".cs", ".gradle", ".css",
         ".dart", ".go", ".rs"}
HASH_EXT = {".cmake", ".py", ".sh", ".properties", ".toml", ".yml", ".yaml", ".pro", ".pri", ".in"}
XML_EXT = {".xml", ".xaml", ".html", ".htm", ".svg", ".storyboard", ".xib", ".plist", ".resx",
           ".manifest", ".axml"}
DATA_EXT = {".json", ".strings", ".stringsdict", ".txt", ".def", ".map", ".rc2", ".ini"}
SKIP_DIRS = {"test", "tests", "doc", "docs", "build", "node_modules", "__pycache__", ".git"}
SKIP_SUFFIX = {".md", ".rst", ".png", ".jpg", ".jpeg", ".gif", ".ico", ".icns", ".webp", ".ttf",
               ".otf", ".woff", ".woff2", ".jar", ".so", ".a", ".o", ".dll", ".exe", ".pdf"}
SOURCE_EXT = C_EXT - {".rc", ".css"}  # files that can call the backend
PREFIX = re.compile(r"bonjour|\bhello\b", re.I)
# loomora_greet (C API, wasm), greet / Greet as a whole word (JNI, Kotlin, Swift, TypeScript wrappers)
GREET_CALL = re.compile(r"(?<![A-Za-z0-9_])(?:loomora_greet|[Gg]reet)(?![A-Za-z0-9_])")


def style_of(path):
    """'c', 'hash', 'xml' or 'data' (no comment syntax); None for a file that is not text we check."""
    ext = path.suffix.lower()
    if path.name == "CMakeLists.txt":
        return "hash"
    if ext in C_EXT:
        return "c"
    if ext in HASH_EXT:
        return "hash"
    if ext in XML_EXT:
        return "xml"
    if ext in DATA_EXT or ext == "":
        return "data"
    return None


def blank_comments(text, style):
    """`text` with every comment replaced by spaces (newlines kept): offsets stay valid."""
    if style == "data":
        return text
    out = list(text)
    n, i = len(text), 0

    def blank(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if style == "c":
            if c == "/" and text.startswith("//", i):
                j = text.find("\n", i)
                j = n if j < 0 else j
                blank(i, j)
                i = j
            elif c == "/" and text.startswith("/*", i):
                j = text.find("*/", i + 2)
                j = n if j < 0 else j + 2
                blank(i, j)
                i = j
            elif c == '"' or c == "'":  # a string or character literal: skip it whole
                i = skip_quoted(text, i)
            else:
                i += 1
        elif style == "hash":
            if c == "#" and (i == 0 or text[i - 1] in " \t\n"):
                j = text.find("\n", i)
                j = n if j < 0 else j
                blank(i, j)
                i = j
            elif c == '"':
                i = skip_quoted(text, i)
            else:
                i += 1
        else:  # xml
            if text.startswith("<!--", i):
                j = text.find("-->", i + 4)
                j = n if j < 0 else j + 3
                blank(i, j)
                i = j
            else:
                i += 1
    return "".join(out)


def skip_quoted(text, i):
    """Index after the quoted literal that starts at text[i] (backslash escapes; stops at a newline)."""
    q, n = text[i], len(text)
    j = i + 1
    while j < n and text[j] != "\n":
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == q:
            return j + 1
        j += 1
    return j


def join_literals(code):
    """(joined text, offsets): adjacent string literals "a" "b" are merged; offsets maps each
    character of the joined text to its offset in `code`."""
    chars, offs, pos = [], [], 0
    for m in re.finditer(r'"\s*"', code):
        if m.start() < pos:
            continue
        for k in range(pos, m.start()):
            chars.append(code[k])
            offs.append(k)
        pos = m.end()
    for k in range(pos, len(code)):
        chars.append(code[k])
        offs.append(k)
    return "".join(chars), offs


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def scan_file(path, root):
    """Returns (violations, comment_hits, calls_greet, is_source) for one file."""
    style = style_of(path)
    if style is None:
        return [], [], False, False
    try:
        text = path.read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return [], [], False, False
    # A UTF-16 .rc is read as UTF-8 text with NULs: drop them so that a literal is still found.
    text = text.replace("\x00", "")
    code = blank_comments(text, style)
    rel = path.relative_to(root).as_posix()
    violations, seen = [], set()
    joined, offs = join_literals(code)
    for m in PREFIX.finditer(joined):
        line = line_of(code, offs[m.start()])
        violations.append("%s:%d: %r in code (%s)" % (rel, line, m.group(0), "the UI builds or holds the greeting prefix"))
        seen.add(line)
    comment_hits = []
    for m in PREFIX.finditer(text):
        line = line_of(text, m.start())
        if code[m.start():m.end()].strip() == "" and line not in seen:
            comment_hits.append("%s:%d: %r in a comment" % (rel, line, m.group(0)))
    return violations, comment_hits, bool(GREET_CALL.search(code)), path.suffix.lower() in SOURCE_EXT


def ui_dirs(ui_root):
    """[(label, directory)]: ui/<variant>/<platform> for every platform directory that holds a file
    other than .gitkeep."""
    found = []
    if not ui_root.is_dir():
        return found
    for variant in sorted(p for p in ui_root.iterdir() if p.is_dir()):
        for plat in sorted(p for p in variant.iterdir() if p.is_dir()):
            if any(f.is_file() and f.name != ".gitkeep" for f in plat.rglob("*")):
                found.append(("%s/%s" % (variant.name, plat.name), plat))
    return found


def files_of(directory):
    for dirpath, dirnames, filenames in os.walk(str(directory)):
        dirnames[:] = sorted(d for d in dirnames if d.lower() not in SKIP_DIRS)
        for name in sorted(filenames):
            p = Path(dirpath) / name
            if name != ".gitkeep" and p.suffix.lower() not in SKIP_SUFFIX:
                yield p


def check(root, verbose=False, out=print):
    """Returns the list of violations; prints one line per UI."""
    root = Path(root)
    uis = ui_dirs(root / "ui")
    problems = []
    if not uis:
        out("ui-logic: no UI under %s" % (root / "ui"))
        return problems
    for label, directory in uis:
        v, comments, greet, nfiles, nsource = [], [], False, 0, 0
        for f in files_of(directory):
            fv, fc, fg, is_src = scan_file(f, root)
            nfiles += style_of(f) is not None
            nsource += is_src
            v += fv
            comments += fc
            greet = greet or (fg and is_src)
        if nsource and not greet:
            v.append("%s: has source files but no reference to the backend greeting call (loomora_greet "
                     "or greet) in code" % label)
        problems += v
        out("%s %-26s %d file(s), %d source file(s), backend greeting call %s, %d comment mention(s)"
            % ("FAIL" if v else "OK  ", label, nfiles, nsource, "yes" if greet else "no", len(comments)))
        for line in v:
            out("     violation: " + line)
        if verbose:
            for line in comments:
                out("     allowed:   " + line)
    return problems


# ---- self-test ---------------------------------------------------------------------------------

SELFTEST_CASES = (
    # (name, files {relative path: text}, expected violations count)
    ("clean UI, Coucou default, comment mention allowed",
     {"ui/modern/p/main.cpp": '// shows "Bonjour " + input\nconst char* d = "Coucou"; int n = loomora_greet(a, b, c, d);\n'}, 0),
    ("literal Bonjour in code",
     {"ui/modern/p/main.cpp": 'int n = loomora_greet(a, b, c, d);\nstd::string s = "Bonjour " + t;\n'}, 1),
    ("literal Bonjour in a wide string",
     {"ui/modern/p/main.cpp": 'int n = loomora_greet(a, b, c, d);\nauto s = L"Bonjour ";\n'}, 1),
    ("split literal",
     {"ui/modern/p/main.cpp": 'int n = loomora_greet(a, b, c, d);\nconst char* s = "Bon"\n   "jour ";\n'}, 1),
    ("hello in code, any case",
     {"ui/modern/p/main.cpp": 'int n = loomora_greet(a, b, c, d);\nconst char* s = "Hello ";\n'}, 1),
    ("Bonjour in a resource string",
     {"ui/legacy/w/a.cpp": "int n = loomora_greet(a);\n", "ui/legacy/w/a.rc": 'STRINGTABLE\nBEGIN\n  1 "Bonjour "\nEND\n'}, 1),
    ("Bonjour in a block comment and after a comment marker in a string",
     {"ui/legacy/w/a.cpp": 'int n = loomora_greet(a); /* "Bonjour " */\nconst char* u = "http://x"; // hello\n'}, 0),
    ("Bonjour after a // that is inside a string is code",
     {"ui/legacy/w/a.cpp": 'int n = loomora_greet(a); const char* s = "//Bonjour ";\n'}, 1),
    ("XML comment allowed, XML text not",
     {"ui/legacy/a/s.cpp": "int n = loomora_greet(a);\n", "ui/legacy/a/x.xml": "<!-- Bonjour -->\n<string>Bonjour</string>\n"}, 1),
    ("CMake comment allowed",
     {"ui/legacy/l/CMakeLists.txt": "# hello Bonjour\nadd_executable(x main.cpp)\n", "ui/legacy/l/main.cpp": "int n = loomora_greet(a);\n"}, 0),
    ("source without a greeting call",
     {"ui/modern/p/main.cpp": "int main() { return 0; }\n"}, 1),
    ("tests and docs skipped",
     {"ui/modern/p/main.cpp": "int n = loomora_greet(a);\n", "ui/modern/p/tests/t.cpp": 'auto s = "Bonjour ";\n',
      "ui/modern/p/README.md": "Bonjour hello\n"}, 0),
    ("empty platform directory (.gitkeep only) is not a UI",
     {"ui/modern/p/.gitkeep": ""}, 0),
)


def selftest():
    failures = 0
    base = Path(tempfile.mkdtemp(prefix="loomora-ui-logic-"))
    try:
        for i, (name, files, expected) in enumerate(SELFTEST_CASES):
            root = base / ("case%d" % i)
            for rel, text in files.items():
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text, encoding="utf-8")
            problems = check(root, out=lambda *_: None)
            good = len(problems) == expected
            failures += not good
            print("%s selftest %-66s %d violation(s), expected %d" % ("OK  " if good else "FAIL", name, len(problems), expected))
    finally:
        shutil.rmtree(str(base), ignore_errors=True)
    print("selftest: %d case(s), %d failed" % (len(SELFTEST_CASES), failures))
    return 1 if failures else 0


def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Check that no UI builds the greeting itself.")
    ap.add_argument("--root", default=str(common.ROOT), help="repository root (default: this repository)")
    ap.add_argument("--verbose", action="store_true", help="also list the allowed mentions in comments")
    ap.add_argument("--selftest", action="store_true", help="check the checker on a temporary tree and exit")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not Path(args.root).is_dir():
        print("check_ui_logic: %s is not a directory" % args.root, file=sys.stderr)
        return 2
    problems = check(args.root, args.verbose)
    print("ui-logic: %s" % ("%d violation(s)" % len(problems) if problems else "no UI builds the greeting itself, no prefix literal outside comments"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
