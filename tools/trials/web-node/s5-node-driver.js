#!/usr/bin/env node
// Spike S5 (docs/implementation/validation/spikes.json): load a Loomora Web module under node,
// call the six exports of the C API and run every greet vector of core/tests/vectors.json.
// Tooling, not evidence (its sha256 may be recorded in a verdict); plain ES5 so that it runs
// with the node of the emsdk and with a current node alike.
//
//   node s5-node-driver.js <path of loomora.js> [vectors.json]
//
// <path> is the MODULARIZE module of bridge/wasm/CMakeLists.txt (factory createLoomora, called
// with no argument; a promise or the module itself, with onRuntimeInitialized, are both handled).
// Prints one line per check ("ok" / "FAIL") and "summary: N passed, M failed"; exit status 1 on a
// failure. The module is called through its raw exports (_loomora_*) and the stack, the way
// ccall does, so what is checked is the export list and the memory handling, not a wrapper.
"use strict";
var fs = require("fs");
var path = require("path");

var modulePath = process.argv[2];
var vectorsPath = process.argv[3] ||
  path.join(__dirname, "..", "..", "..", "core", "tests", "vectors.json");
if (!modulePath) {
  console.error("usage: node s5-node-driver.js <loomora.js> [vectors.json]");
  process.exit(2);
}

var passed = 0, failed = 0;
function check(ok, what) {
  if (ok) { passed++; console.log("ok   " + what); }
  else { failed++; console.log("FAIL " + what); }
}

// An 'enc' of vectors.json: a string, {repeat, count}, {hex}, or a list of encs; null = NULL.
function encode(enc) {
  if (typeof enc === "string") return Buffer.from(enc, "utf8");
  if (Array.isArray(enc)) return Buffer.concat(enc.map(encode));
  if (enc.repeat !== undefined) {
    var one = encode(enc.repeat), parts = [];
    for (var i = 0; i < enc.count; i++) parts.push(one);
    return Buffer.concat(parts);
  }
  return Buffer.from(enc.hex.replace(/\s+/g, ""), "hex");
}

function start(factory, done) {
  var m = factory();
  if (m && typeof m.then === "function") { m.then(done); return; }
  if (m._loomora_abi_version) { done(m); return; }
  m.onRuntimeInitialized = function () { done(m); };
}

function run(M) {
  var heap = function () { return M.HEAPU8 || (M.HEAPU8 = new Uint8Array(M.wasmMemory.buffer)); };
  function alloc(n) { return M.stackAlloc ? M.stackAlloc(n) : M._malloc(n); }
  function utf8(ptr) {
    var h = heap(), end = ptr;
    while (h[end] !== 0) end++;
    return Buffer.from(h.subarray(ptr, end)).toString("utf8");
  }
  function greet(bytes, nullInput, inputLen, cap) {
    var sp = M.stackSave ? M.stackSave() : 0;
    var inPtr = nullInput ? 0 : alloc(bytes.length + 1);
    var outPtr = alloc(cap + 1);
    if (!nullInput) heap().set(bytes, inPtr);
    var n = M._loomora_greet(inPtr, inputLen, outPtr, cap);
    var text = n >= 0 ? Buffer.from(heap().subarray(outPtr, outPtr + n)).toString("utf8") : null;
    if (M.stackRestore) M.stackRestore(sp);
    return { status: n, text: text };
  }

  var names = ["abi_version", "greet", "is_eol", "eol_warning", "build_compiler", "probe_record"];
  for (var k = 0; k < names.length; k++)
    check(typeof M["_loomora_" + names[k]] === "function", "export _loomora_" + names[k]);
  check(typeof M.__Znwm === "undefined" && typeof M._main === "undefined",
        "no other C++ or main symbol exported");

  console.log("info abi_version=" + M._loomora_abi_version());
  console.log("info build_compiler=" + utf8(M._loomora_build_compiler()));
  console.log("info eol_warning=" + JSON.stringify(utf8(M._loomora_eol_warning())));
  check(M._loomora_is_eol(6, 0, 0) === 0, "is_eol(WEB, 0, 0) == 0");
  check(M._loomora_is_eol(1, 5, 0) === 1, "is_eol(WINDOWS, 5, 0) == 1");

  var coucou = greet(Buffer.from("Coucou"), false, 6, 5000);
  console.log("info greet(\"Coucou\") -> " + JSON.stringify(coucou));
  check(coucou.status === 14 && coucou.text === "Bonjour Coucou", "loomora_greet(\"Coucou\") == \"Bonjour Coucou\"");

  var vectors = JSON.parse(fs.readFileSync(vectorsPath, "utf8"));
  vectors.greet.cases.forEach(function (c) {
    var nul = c.input === null;
    var bytes = nul ? Buffer.alloc(0) : encode(c.input);
    var cap = c.capacity !== undefined ? c.capacity : vectors.greet.default_capacity;
    var r = greet(bytes, nul, nul ? c.input_len : bytes.length, cap);
    if (c.error !== undefined) {
      check(r.status === c.error, "vector " + c.id + " -> " + c.error + " (got " + r.status + ")");
    } else {
      var want = encode(c.output).toString("utf8");
      check(r.status === Buffer.byteLength(want) && r.text === want, "vector " + c.id);
    }
  });

  // probe record with core_kind 3 (cpp-js) and 2 (cpp-wasm): one ASCII JSON line.
  var rssForm = "?";
  function probe(kind) {
    var sp = M.stackSave ? M.stackSave() : 0;
    function str(s) { var b = Buffer.from(s + "\0"); var p = alloc(b.length); heap().set(b, p); return p; }
    var buf = alloc(2048);
    // rss_kb is an int64 (-1 = not measured). A WebAssembly build with WASM_BIGINT exports it as
    // one BigInt parameter (13 parameters); the JavaScript builds (and WebAssembly without
    // WASM_BIGINT) export it as two 32-bit numbers, low then high (14 parameters). The wrappers
    // of old Emscripten versions hide the parameter count (length 0), so each form is tried, the
    // likely one first, and the first one that returns a plausible length is kept.
    var forms = [["two 32-bit numbers (low, high)", [-1, -1]], ["one BigInt", [BigInt(-1)]]];
    if (M._loomora_probe_record.length === 13) forms.reverse();
    var n = -1;
    for (var f = 0; f < forms.length && !(n > 0 && n <= 2048); f++) {
      try {
        n = M._loomora_probe_record.apply(null,
          [str("web-node"), 1, kind, str("Coucou"), str("Bonjour Coucou"), 1, 5].concat(
            forms[f][1], [0, str(""), str("2026-11-15T00:00:00Z"), buf, 2048]));
        if (n > 0 && n <= 2048) rssForm = forms[f][0];
      } catch (e) { n = -1; }
    }
    var line = n > 0 && n <= 2048 ? Buffer.from(heap().subarray(buf, buf + n)).toString("ascii") : "";
    if (M.stackRestore) M.stackRestore(sp);
    return line;
  }
  [3, 2].forEach(function (kind) {
    var line = probe(kind), ok = false;
    try { var j = JSON.parse(line); ok = j.greeting_matches === true; } catch (e) { ok = false; }
    check(ok, "probe_record core_kind " + kind + " is JSON with greeting_matches true");
  });

  console.log("info rss_kb (int64) is passed as a " + rssForm);
  console.log("summary: " + passed + " passed, " + failed + " failed");
  process.exit(failed ? 1 : 0);
}

// LOOMORA_NO_WASM=1 removes the WebAssembly global first: a module that still starts proves it
// does not use WebAssembly (the -sWASM=0 build).
if (process.env.LOOMORA_NO_WASM) {
  delete globalThis.WebAssembly;
  console.log("info WebAssembly global removed: typeof WebAssembly = " + typeof WebAssembly);
}

var factory = require(path.resolve(modulePath));
start(factory, run);
