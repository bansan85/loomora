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
//
// Environment variables (all optional), to run the module in less than a current engine:
//   LOOMORA_NO_WASM=1          delete the WebAssembly global before loading the module.
//   LOOMORA_NO_TYPED_ARRAYS=1  also delete every typed array global (ArrayBuffer, Int8Array ...
//                              Float64Array, DataView, ...): Internet Explorer 6 to 9 have none (spike
//                              ES3). The module's memory (M.HEAPU8) is read and written by index only.
//   LOOMORA_ES5=off            delete the ES5 library additions (Object.defineProperty,
//                              Function.prototype.bind, Array.prototype.indexOf/forEach/map/...,
//                              Array.isArray, Date.now, JSON, ...) before loading the module: a module
//                              that needs one fails at that point.
//   LOOMORA_ES5=trace          keep them but wrap each one, and print at the end which ones the module
//                              (not this driver, which uses copies taken before) called, and how often.
"use strict";
var fs = require("fs");
var path = require("path");
// Copies of what this driver itself needs, taken before any ES5 addition can be removed.
var JSONcopy = { parse: JSON.parse, stringify: JSON.stringify }, isArray = Array.isArray, BufferCopy = Buffer;
var BigIntCopy = typeof BigInt === "function" ? BigInt : null;
var definePropertyCopy = Object.defineProperty;

var modulePath = process.argv[2];
var vectorsPath = process.argv[3] ||
  path.join(__dirname, "..", "..", "..", "core", "tests", "vectors.json");
if (!modulePath) {
  console.error("usage: node s5-node-driver.js <loomora.js> [vectors.json]");
  process.exit(2);
}
// Read here, before anything is removed.
var vectorsParsed = JSONcopy.parse(fs.readFileSync(vectorsPath, "utf8"));

var passed = 0, failed = 0;
function check(ok, what) {
  if (ok) { passed++; console.log("ok   " + what); }
  else { failed++; console.log("FAIL " + what); }
}

// An 'enc' of vectors.json: a string, {repeat, count}, {hex}, or a list of encs; null = NULL.
function encode(enc) {
  if (typeof enc === "string") return BufferCopy.from(enc, "utf8");
  if (isArray(enc)) {
    var list = [];
    for (var j = 0; j < enc.length; j++) list.push(encode(enc[j]));
    return BufferCopy.concat(list);
  }
  if (enc.repeat !== undefined) {
    var one = encode(enc.repeat), parts = [];
    for (var i = 0; i < enc.count; i++) parts.push(one);
    return BufferCopy.concat(parts);
  }
  return BufferCopy.from(enc.hex.replace(/\s+/g, ""), "hex");
}

function start(factory, done) {
  var m = factory();
  if (m && typeof m.then === "function") { m.then(done); return; }
  if (m._loomora_abi_version) { done(m); return; }
  m.onRuntimeInitialized = function () { done(m); };
}

var traced = {};
function report() {
  var names = [], n, i;
  for (n in traced) names.push(n);
  names.sort();
  if (process.env.LOOMORA_ES5 === "trace")
    console.log("info ES5 library features the module called: " + (names.length ? names.join(", ") : "none"));
  for (i = 0; i < names.length; i++)
    console.log("info   " + names[i] + ": " + traced[names[i]].count + " call(s), first from " + traced[names[i]].where);
}

function run(M) {
  var heap = function () { return M.HEAPU8 || (M.HEAPU8 = new Uint8Array(M.wasmMemory.buffer)); };
  function alloc(n) { return M.stackAlloc ? M.stackAlloc(n) : M._malloc(n); }
  // The heap is read and written byte by byte, by index only: that works on a typed array and on the
  // plain array of a build without typed arrays alike.
  function readBytes(from, to) {
    var h = heap(), a = [];
    for (var i = from; i < to; i++) a.push(h[i]);
    return BufferCopy.from(a);
  }
  function writeBytes(bytes, at) {
    var h = heap();
    for (var i = 0; i < bytes.length; i++) h[at + i] = bytes[i];
  }
  function utf8(ptr) {
    var h = heap(), end = ptr;
    while (h[end] !== 0) end++;
    return readBytes(ptr, end).toString("utf8");
  }
  function greet(bytes, nullInput, inputLen, cap) {
    var sp = M.stackSave ? M.stackSave() : 0;
    var inPtr = nullInput ? 0 : alloc(bytes.length + 1);
    var outPtr = alloc(cap + 1);
    if (!nullInput) writeBytes(bytes, inPtr);
    var n = M._loomora_greet(inPtr, inputLen, outPtr, cap);
    var text = n >= 0 ? readBytes(outPtr, outPtr + n).toString("utf8") : null;
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
  console.log("info eol_warning=" + JSONcopy.stringify(utf8(M._loomora_eol_warning())));
  check(M._loomora_is_eol(6, 0, 0) === 0, "is_eol(WEB, 0, 0) == 0");
  check(M._loomora_is_eol(1, 5, 0) === 1, "is_eol(WINDOWS, 5, 0) == 1");

  var coucou = greet(BufferCopy.from("Coucou"), false, 6, 5000);
  console.log("info greet(\"Coucou\") -> " + JSONcopy.stringify(coucou));
  check(coucou.status === 14 && coucou.text === "Bonjour Coucou", "loomora_greet(\"Coucou\") == \"Bonjour Coucou\"");

  var vectors = vectorsParsed;
  for (var vi = 0; vi < vectors.greet.cases.length; vi++) {
    var c = vectors.greet.cases[vi];
    var nul = c.input === null;
    var bytes = nul ? BufferCopy.alloc(0) : encode(c.input);
    var cap = c.capacity !== undefined ? c.capacity : vectors.greet.default_capacity;
    var r = greet(bytes, nul, nul ? c.input_len : bytes.length, cap);
    if (c.error !== undefined) {
      check(r.status === c.error, "vector " + c.id + " -> " + c.error + " (got " + r.status + ")");
    } else {
      var want = encode(c.output).toString("utf8");
      check(r.status === BufferCopy.byteLength(want) && r.text === want, "vector " + c.id);
    }
  }

  // probe record with core_kind 3 (cpp-js) and 2 (cpp-wasm): one ASCII JSON line.
  var rssForm = "?";
  function probe(kind) {
    var sp = M.stackSave ? M.stackSave() : 0;
    function str(s) { var b = BufferCopy.from(s + "\0"); var p = alloc(b.length); writeBytes(b, p); return p; }
    var buf = alloc(2048);
    // rss_kb is an int64 (-1 = not measured). A WebAssembly build with WASM_BIGINT exports it as
    // one BigInt parameter (13 parameters); the JavaScript builds (and WebAssembly without
    // WASM_BIGINT) export it as two 32-bit numbers, low then high (14 parameters). The wrappers
    // of old Emscripten versions hide the parameter count (length 0), so each form is tried, the
    // likely one first, and the first one that returns a plausible length is kept.
    var forms = [["two 32-bit numbers (low, high)", [-1, -1]]];
    if (BigIntCopy) forms.push(["one BigInt", [BigIntCopy(-1)]]);
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
    var line = n > 0 && n <= 2048 ? readBytes(buf, buf + n).toString("ascii") : "";
    if (M.stackRestore) M.stackRestore(sp);
    return line;
  }
  var kinds = [3, 2];
  for (var ki = 0; ki < kinds.length; ki++) {
    var kind = kinds[ki], line = probe(kind), ok = false;
    try { var j = JSONcopy.parse(line); ok = j.greeting_matches === true; } catch (e) { ok = false; }
    check(ok, "probe_record core_kind " + kind + " is JSON with greeting_matches true");
  }

  console.log("info rss_kb (int64) is passed as a " + rssForm);
  console.log("summary: " + passed + " passed, " + failed + " failed");
  report();
  process.exit(failed ? 1 : 0);
}

// The ES5 library additions (nothing here is syntax: es-level.js checks that the file parses as ES3).
function es5Table() {
  var t = [];
  function add(owner, ownerName, list) {
    for (var i = 0; i < list.length; i++) t.push({ owner: owner, ownerName: ownerName, name: list[i] });
  }
  add(Object, "Object", ["defineProperty", "defineProperties", "create", "keys", "getPrototypeOf",
    "getOwnPropertyNames", "getOwnPropertyDescriptor", "freeze", "seal", "preventExtensions", "isFrozen",
    "isSealed", "isExtensible"]);
  add(Function.prototype, "Function.prototype", ["bind"]);
  add(Array, "Array", ["isArray"]);
  add(Array.prototype, "Array.prototype", ["indexOf", "lastIndexOf", "forEach", "map", "filter", "some",
    "every", "reduce", "reduceRight"]);
  add(String.prototype, "String.prototype", ["trim"]);
  add(Date, "Date", ["now"]);
  add(Date.prototype, "Date.prototype", ["toISOString", "toJSON"]);
  add(JSON, "JSON", ["parse", "stringify"]);
  return t;
}
function applyEs5(mode) {
  var table = es5Table();
  for (var i = 0; i < table.length; i++) {
    (function (e) {
      var original = e.owner[e.name], label = e.ownerName + "." + e.name;
      if (typeof original !== "function") return;
      if (mode === "off") {
        // This driver keeps its own copies of what it needs (JSONcopy, isArray, BufferCopy).
        delete e.owner[e.name];
      } else {
        definePropertyCopy(e.owner, e.name, { configurable: true, writable: true, enumerable: false,
          value: function () {
            var rec = traced[label] || (traced[label] = { count: 0, where: "" });
            if (!rec.count) rec.where = String(new Error().stack).split("\n")[2] || "";
            rec.count++;
            return original.apply(this, arguments);
          } });
      }
    })(table[i]);
  }
  if (mode === "off") {
    delete globalThis.JSON;
    console.log("info ES5 library additions removed: typeof JSON = " + typeof JSON +
                ", typeof Object.defineProperty = " + typeof Object.defineProperty +
                ", typeof Function.prototype.bind = " + typeof Function.prototype.bind +
                ", typeof Array.prototype.forEach = " + typeof Array.prototype.forEach +
                ", typeof Date.now = " + typeof Date.now);
  }
}

// LOOMORA_NO_WASM=1 removes the WebAssembly global first: a module that still starts proves it
// does not use WebAssembly (the -sWASM=0 build).
if (process.env.LOOMORA_NO_WASM || process.env.LOOMORA_NO_TYPED_ARRAYS) {
  delete globalThis.WebAssembly;
  console.log("info WebAssembly global removed: typeof WebAssembly = " + typeof WebAssembly);
}
// LOOMORA_NO_TYPED_ARRAYS=1: node itself keeps working (its Buffer was built before this point).
if (process.env.LOOMORA_NO_TYPED_ARRAYS) {
  var typed = ["ArrayBuffer", "SharedArrayBuffer", "DataView", "Int8Array", "Uint8Array", "Uint8ClampedArray",
    "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
    "BigInt64Array", "BigUint64Array", "TextDecoder", "TextEncoder", "Atomics"];
  var left = [];
  for (var ti = 0; ti < typed.length; ti++) {
    delete globalThis[typed[ti]];
    if (typeof globalThis[typed[ti]] !== "undefined") left.push(typed[ti]);
  }
  console.log("info typed arrays removed: typeof ArrayBuffer = " + typeof ArrayBuffer + ", typeof Int8Array = " +
              typeof Int8Array + ", typeof Uint8Array = " + typeof Uint8Array + ", typeof Int16Array = " +
              typeof Int16Array + ", typeof Uint16Array = " + typeof Uint16Array + ", typeof Int32Array = " +
              typeof Int32Array + ", typeof Uint32Array = " + typeof Uint32Array + ", typeof Float32Array = " +
              typeof Float32Array + ", typeof Float64Array = " + typeof Float64Array + ", typeof DataView = " +
              typeof DataView + (left.length ? "; NOT removed: " + left.join(",") : ""));
}
if (process.env.LOOMORA_ES5 === "off" || process.env.LOOMORA_ES5 === "trace") applyEs5(process.env.LOOMORA_ES5);

var factory = require(path.resolve(modulePath));
start(factory, run);
