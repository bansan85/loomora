/* Loomora Web Legacy UI: the only script of index.html.
 *
 * It has no application logic: it loads the backend (the C++11 library libloomora built by
 * Emscripten), asks it for the greeting through ccall / cwrap (loomora_greet) and shows the
 * result. The source stays inside what Firefox 8 to 12 (and every engine with ECMAScript 3
 * syntax, typed arrays and the ES5 library functions of that time) can parse and run: var and
 * function only, no arrow function, let / const, template string, class, Object.assign,
 * Promise, fetch, trailing comma or reserved word used as a property name. tools/build.py
 * checks that the file parses as ECMAScript 3 (tools/trials/web-node/es-level.js).
 *
 * Loading (research R8): a browser that has WebAssembly (typeof WebAssembly === "object") loads
 * loomora-wasm.js (+ loomora.wasm, Emscripten 7.0.0); any other loads loomora.js (JavaScript
 * only, asm.js, Emscripten 1.39.20 fastcomp).
 *
 * Probe mode (ui-contract.md, SC-011): index.html?probe=1&input=<percent-encoded UTF-8>
 *   probe=1      probe mode (any value; the parameter alone also works)
 *   input=TEXT   the text put in the field instead of "Coucou" (cut to 1,000 UTF-16 units)
 *   platform=ID  the platform_id of the probe record (default "web-unknown")
 *   path=js      use the JavaScript build even in a browser that has WebAssembly
 * The probe record (one ASCII JSON line, loomora_probe_record) is written in <pre id="probe">
 * and to console.log, and document.title becomes LOOMORA_PROBE_DONE, or
 * "LOOMORA_PROBE_ERROR: <message>" when something failed. '+' in a value is a plus sign.
 */
(function () {
  "use strict";

  var MAX_CHARS = 1000;         /* the field limit of ui-contract.md, in UTF-16 units */
  var GREET_CAP = 4200;         /* 4,000 bytes at most (1,000 units) + "Bonjour " + room */
  var PROBE_CAP = 16384;        /* the record is ASCII, at most about 14,000 bytes */
  var ABI_VERSION = 1;
  var FAMILY_WEB = 6, UI_LEGACY = 1, CORE_WASM = 2, CORE_JS = 3;
  var LOAD_TIMEOUT_MS = 60000;

  var startTime = new Date().getTime();
  var params = {};
  var probeMode = false;
  var webPath = "js";
  var core = null;              /* what bindCore() makes of the module */
  var finished = false;       /* the backend started and the first greeting is shown */
  var failed = false;
  var lastText = null;
  var nameEl = null, greetingEl = null, statusEl = null, probeEl = null;

  function el(id) {
    return document.getElementById(id);
  }

  function setText(node, text) {
    while (node.firstChild) {
      node.removeChild(node.firstChild);
    }
    node.appendChild(document.createTextNode(text));
  }

  function textOf(node) {
    return node.firstChild ? node.firstChild.nodeValue : "";
  }

  /* ---- URL parameters ---------------------------------------------------------------- */

  function parseQuery(search) {
    var result = {}, text = search.charAt(0) === "?" ? search.substring(1) : search;
    var parts, i, pair, eq;
    if (text === "") {
      return result;
    }
    parts = text.split("&");
    for (i = 0; i < parts.length; i++) {
      pair = parts[i];
      if (pair === "") {
        continue;
      }
      eq = pair.indexOf("=");
      /* the "p:" prefix keeps a name such as "toString" from meaning something else */
      if (eq < 0) {
        result["p:" + decodeURIComponent(pair)] = "";
      } else {
        result["p:" + decodeURIComponent(pair.substring(0, eq))] = decodeURIComponent(pair.substring(eq + 1));
      }
    }
    return result;
  }

  function param(name) {
    return Object.prototype.hasOwnProperty.call(params, "p:" + name) ? params["p:" + name] : null;
  }

  /* The field limit, never cutting a surrogate pair in two. */
  function limitText(text) {
    var cut, last;
    if (text.length <= MAX_CHARS) {
      return text;
    }
    cut = MAX_CHARS;
    last = text.charCodeAt(cut - 1);
    if (last >= 0xD800 && last <= 0xDBFF) {
      cut--;
    }
    return text.substring(0, cut);
  }

  /* ---- encoding: JavaScript string <-> bytes for the backend --------------------------- */

  /* UTF-8 bytes of a string (a lone surrogate becomes U+FFFD, as TextEncoder does). */
  function utf8Bytes(text) {
    var bytes = [], i, c, d;
    for (i = 0; i < text.length; i++) {
      c = text.charCodeAt(i);
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.length) {
        d = text.charCodeAt(i + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) {
          c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
          i++;
        }
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
        c = 0xFFFD;
      }
      if (c < 0x80) {
        bytes.push(c);
      } else if (c < 0x800) {
        bytes.push(0xC0 | (c >> 6), 0x80 | (c & 63));
      } else if (c < 0x10000) {
        bytes.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      } else {
        bytes.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      }
    }
    return bytes;
  }

  /* ---- the backend ------------------------------------------------------------------- */

  /* A NUL-terminated copy of `text` on the module's stack; returns its address. */
  function cString(M, text) {
    var bytes = utf8Bytes(text), ptr = M.stackAlloc(bytes.length + 1), heap = M.HEAPU8, i;
    for (i = 0; i < bytes.length; i++) {
      heap[ptr + i] = bytes[i];
    }
    heap[ptr + bytes.length] = 0;
    return ptr;
  }

  function bindCore(M, path) {
    var num = "number";
    var greetFn, isEolFn, probeFn, wide, abi;
    abi = M.ccall("loomora_abi_version", num, [], []);
    if (abi !== ABI_VERSION) {
      throw new Error("the backend has ABI version " + abi + ", this page needs " + ABI_VERSION);
    }
    greetFn = M.cwrap("loomora_greet", num, [num, num, num, num]);
    isEolFn = M.cwrap("loomora_is_eol", num, [num, num, num]);
    /* The int64 rss_kb of loomora_probe_record: one BigInt parameter in the WebAssembly build
       (13 parameters), two 32-bit numbers, low then high, in the JavaScript builds (14). */
    wide = path === "wasm" && typeof BigInt === "function" && M._loomora_probe_record.length === 13;
    probeFn = M.cwrap("loomora_probe_record", num,
      wide ? [num, num, num, num, num, num, num, num, num, num, num, num, num]
           : [num, num, num, num, num, num, num, num, num, num, num, num, num, num]);

    return {
      greet: function (text) {
        var bytes = utf8Bytes(text), sp = M.stackSave(), inPtr, outPtr, heap, i, n;
        try {
          inPtr = M.stackAlloc(bytes.length + 1);
          outPtr = M.stackAlloc(GREET_CAP + 1);
          heap = M.HEAPU8;
          for (i = 0; i < bytes.length; i++) {
            heap[inPtr + i] = bytes[i];
          }
          n = greetFn(inPtr, bytes.length, outPtr, GREET_CAP);
          if (n < 0) {
            throw new Error("loomora_greet failed with status " + n);
          }
          M.HEAPU8[outPtr + n] = 0;   /* read again: the heap may have moved while the backend ran */
          return M.UTF8ToString(outPtr);
        } finally {
          M.stackRestore(sp);
        }
      },

      isEol: function (family, major, minor) {
        return isEolFn(family, major, minor);
      },

      eolWarning: function () {
        return M.UTF8ToString(M.ccall("loomora_eol_warning", num, [], []));
      },

      /* rssKb < 0: not measured. Returns the JSON line, or "" if the backend refuses. */
      probe: function (platformId, coreKind, input, shown, displayed, displayMs, rssKb, eolShown, stamp) {
        var sp = M.stackSave(), cap = PROBE_CAP, buf, args, need, heap, i, line;
        try {
          for (;;) {
            buf = M.stackAlloc(cap);
            args = [cString(M, platformId), UI_LEGACY, coreKind, cString(M, input), cString(M, shown),
                    displayed, displayMs];
            if (wide) {
              args.push(BigInt(rssKb));
            } else {
              args.push(rssKb < 0 ? rssKb : rssKb % 4294967296, rssKb < 0 ? -1 : 0);
            }
            args.push(eolShown, cString(M, ""), cString(M, stamp), buf, cap);
            need = probeFn.apply(null, args);
            if (need <= cap) {
              break;
            }
            cap = need;   /* the return value is the length needed: ask again with room */
          }
          heap = M.HEAPU8;
          line = "";
          for (i = 0; i < need; i++) {
            line += String.fromCharCode(heap[buf + i]);
          }
          return line;
        } finally {
          M.stackRestore(sp);
        }
      }
    };
  }

  /* ---- loading ----------------------------------------------------------------------- */

  function loadScript(src, onLoaded, onFailed) {
    var script = document.createElement("script"), done = false;
    function ok() {
      if (!done) {
        done = true;
        onLoaded();
      }
    }
    script.type = "text/javascript";
    script.onload = ok;
    script.onerror = function () {
      if (!done) {
        done = true;
        onFailed("cannot load " + src);
      }
    };
    /* an old Internet Explorer has no onload for scripts, only readyState */
    script.onreadystatechange = function () {
      if (script.readyState === "loaded" || script.readyState === "complete") {
        ok();
      }
    };
    script.src = src;
    (document.getElementsByTagName("head")[0] || document.documentElement).appendChild(script);
  }

  /* createLoomora() returns a Promise (Emscripten 7.0.0) or the module itself, with a then()
     of its own that waits for the runtime (1.39.20): either way then() hands over the module. */
  function startModule(onReady, onFailed) {
    var module;
    if (typeof createLoomora !== "function") {
      onFailed("the backend script did not define createLoomora");
      return;
    }
    module = createLoomora({});
    if (module && typeof module.then === "function") {
      module.then(function (m) {
        onReady(m || module);
      }, function (e) {
        onFailed("the backend did not start: " + e);
      });
    } else {
      onReady(module);
    }
  }

  /* ---- display ----------------------------------------------------------------------- */

  function showGreeting(text) {
    setText(greetingEl, core.greet(text));
    lastText = text;
  }

  function onFieldChange() {
    var text;
    if (!core || failed) {
      return;
    }
    text = nameEl.value;
    if (text !== lastText) {
      try {
        showGreeting(text);
      } catch (e) {
        fail(e);
      }
    }
  }

  /* ---- probe ------------------------------------------------------------------------- */

  function two(n) {
    return n < 10 ? "0" + n : "" + n;
  }

  function timestampUtc() {
    var d = new Date();
    return d.getUTCFullYear() + "-" + two(d.getUTCMonth() + 1) + "-" + two(d.getUTCDate()) + "T" +
           two(d.getUTCHours()) + ":" + two(d.getUTCMinutes()) + ":" + two(d.getUTCSeconds()) + "Z";
  }

  /* Best effort (ui-contract.md): performance.memory where the browser has it (Chrome),
     in KiB; -1 = not measured. It is the JavaScript heap in use, not a peak. */
  function memoryKb() {
    var mem = typeof performance !== "undefined" ? performance.memory : null;
    if (mem && typeof mem.usedJSHeapSize === "number" && mem.usedJSHeapSize > 0) {
      return Math.floor(mem.usedJSHeapSize / 1024);
    }
    return -1;
  }

  function writeProbe(eolShown) {
    var line = core.probe(param("platform") || "web-unknown", webPath === "wasm" ? CORE_WASM : CORE_JS,
                          nameEl.value, textOf(greetingEl), 1, new Date().getTime() - startTime,
                          memoryKb(), eolShown ? 1 : 0, timestampUtc());
    if (line === "") {
      throw new Error("loomora_probe_record returned nothing");
    }
    setText(probeEl, line);
    probeEl.style.display = "block";
    if (typeof console !== "undefined" && console.log) {
      console.log(line);
    }
    document.title = "LOOMORA_PROBE_DONE";
  }

  /* ---- errors ------------------------------------------------------------------------ */

  function fail(error) {
    var message = error && error.message ? error.message : String(error);
    if (failed) {
      return;
    }
    failed = true;
    if (probeMode) {
      document.title = "LOOMORA_PROBE_ERROR: " + message;
    }
    if (statusEl) {
      setText(statusEl, "Error: " + message);
    }
    if (typeof console !== "undefined" && console.log) {
      console.log("LOOMORA_ERROR: " + message);
    }
  }

  /* ---- start ------------------------------------------------------------------------- */

  function ready(M) {
    var eol = 0, eolEl, text;
    core = bindCore(M, webPath);
    /* The family and the version are all the page tells the backend: a browser cannot read the
       system version, and the backend answers 0 for the Web family; a 1 would show the
       warning. */
    eol = core.isEol(FAMILY_WEB, 0, 0);
    if (eol === 1) {
      eolEl = document.createElement("div");
      eolEl.id = "eol";
      setText(eolEl, core.eolWarning());
      el("page").appendChild(eolEl);
    }
    text = nameEl.value;
    showGreeting(text);
    setText(statusEl, "");
    nameEl.onkeyup = onFieldChange;
    nameEl.oninput = onFieldChange;
    nameEl.onchange = onFieldChange;
    nameEl.onpropertychange = onFieldChange;   /* Internet Explorer */
    if (probeMode) {
      writeProbe(eol === 1);
    }
    finished = true;
  }

  function main() {
    var src, input;
    nameEl = el("name");
    greetingEl = el("greeting");
    statusEl = el("status");
    probeEl = el("probe");
    try {
      /* decided before the parsing, so that a malformed URL is still reported as a probe error */
      probeMode = /[?&]probe([=&]|$)/.test(window.location.search || "");
      params = parseQuery(window.location.search || "");
      if (probeMode) {
        input = param("input");
        if (input !== null) {
          nameEl.value = limitText(input);
        }
      }
      webPath = typeof WebAssembly === "object" && param("path") !== "js" ? "wasm" : "js";
      src = webPath === "wasm" ? "loomora-wasm.js" : "loomora.js";
    } catch (e) {
      fail(e);
      return;
    }
    setTimeout(function () {
      if (!finished && !failed) {
        fail("timeout: the backend did not start in " + (LOAD_TIMEOUT_MS / 1000) + " s");
      }
    }, LOAD_TIMEOUT_MS);
    loadScript(src, function () {
      try {
        startModule(function (M) {
          try {
            ready(M);
          } catch (e) {
            fail(e);
          }
        }, fail);
      } catch (e) {
        fail(e);
      }
    }, fail);
  }

  main();
}());
