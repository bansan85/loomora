"""Writes hard.json, a vectors file in the format of core/tests/vectors.json (greet.cases only) with
the hardest texts for a command line: emoji (also a ZWJ sequence and flags), CJK, right-to-left,
combining marks, quotes and backslashes in every position, %VARIABLES%, shell metacharacters,
leading and trailing spaces, option-like text, tab and newline, control characters, line and
paragraph separators, a byte order mark, a soft hyphen, BMP edge code points, and long texts
(1,100 and 1,500 characters, 1,003 UTF-16 units with an emoji at the limit).

  python roundtrip-vectors-gen.py hard.json
  python tools/run_vectors.py windows-2000-x86 legacy --vectors hard.json
"""
import json, sys
cases = {
    "empty": "",
    "marie": "Marie",
    "emoji": "\U0001F600\U0001F389 \U0001F468\u200d\U0001F469\u200d\U0001F467 \U0001F1EB\U0001F1F7",
    "cjk": "\u65e5\u672c\u8a9e\u4e2d\u6587\ud55c\uad6d\uc5b4",
    "rtl": "\u05e9\u05dc\u05d5\u05dd \u0645\u0631\u062d\u0628\u0627 \u200f abc",
    "combining": "e\u0301a\u0308n\u0303 \u0061\u0323\u0302",
    "quotes": 'a "b" \'c\' "',
    "only_quote": '"',
    "two_quotes": '""',
    "backslashes": 'C:\\dir\\sub\\',
    "bs_quote": 'a\\"b\\\\"c\\\\\\"d',
    "trailing_bs": "x\\\\",
    "percent": "%PATH% %%  %USERNAME% %1 %*",
    "shell_meta": "^&|<>!() ; , = ~ ` $",
    "spaces": "   lead and trail   ",
    "only_space": " ",
    "looks_like_option": "--probe --probe-input /c dir",
    "tab_nl": "a\tb\nc\r\nd",
    "ctrl": "\x01\x02\x1f\x7f\x85",
    "separators": "a\u2028b\u2029c\ufeffd\u00a0e\u3000f\u00adg",
    "bmp_edge": "\ud7ff\ue000\uffe0",
    "long_1100_e_acute": "\u00e9" * 1100,
    "long_1000_cjk": "\u4e2d" * 1000,
    "long_1500_quote_bs": ('\\"' * 750),
    "long_ascii_with_emoji": "a" * 998 + "\U0001F600" + "tail",
}
out = {"greet": {"max_input_bytes": 4096, "prefix": "Bonjour ", "cases": []}}
for k, v in cases.items():
    out["greet"]["cases"].append({"id": k, "input": {"hex": v.encode("utf-8").hex()},
                                  "output": {"hex": ("Bonjour " + v).encode("utf-8").hex()}})
json.dump(out, open(sys.argv[1], "w"), indent=1)
print(len(cases))
