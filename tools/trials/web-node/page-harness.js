#!/usr/bin/env node
// Runs the Web page and the built backend under node, in a vm context with a minimal DOM stub,
// without any browser: the Legacy page (ui/legacy/web: index.html + app.js) or the Modern page
// (ui/modern/web-ts: index.html + the ES modules tsc writes). Tooling, not evidence.
//
//   node [--experimental-vm-modules] page-harness.js [--variant legacy|modern] [--dist DIR]
//        [--path wasm|js] [--query "?probe=1&input=Coucou"|-] [--typing TEXT] [--ua USER-AGENT]
//        [--timeout SECONDS] [--streaming present|absent|bad-mime] [--verbose]
//
//   --variant V    legacy (default) or modern. The Modern page is an ES module: node needs
//                  --experimental-vm-modules to run it (vm.SourceTextModule); it only has the wasm path.
//   --dist DIR     the folder tools/build.py web legacy (or web modern) assembles (default
//                  $LOOMORA_WEB_DIST, then $HOME/loomora-build/web-<variant>/dist): index.html, app.js,
//                  loomora.js, loomora-wasm.js, loomora.wasm (Legacy); index.html, main.js, native.js,
//                  loomora.js, loomora.wasm (Modern).
//   --streaming S  modern only: present (default) gives the page the real WebAssembly.instantiateStreaming
//                  and a Response of type application/wasm; absent removes instantiateStreaming (the
//                  ArrayBuffer form is used); bad-mime serves the module as application/octet-stream, so
//                  that instantiateStreaming rejects and the page falls back to the ArrayBuffer form.
//   --path P       wasm: the context has WebAssembly (the page loads loomora-wasm.js and loomora.wasm);
//                  js (default): WebAssembly, BigInt and Promise are deleted from the context before
//                  the page runs, as in an engine that has none of them (the page loads loomora.js).
//   --query Q      the URL query of the page; "-" reads it from standard input (ASCII, so that any
//                  text can be passed through a command line that mangles non-ASCII). Without
//                  --query the page runs without probe mode.
//   --ua STRING    navigator.userAgent of the context (default "node page-harness"); for example
//                  "Mozilla/5.0 (Windows NT 5.0; rv:10.0) Gecko/20100101 Firefox/10.0" shows the
//                  end-of-life notice (the harness then checks that the record and the page agree).
//   --typing TEXT  after the first greeting, set the field to TEXT and fire onkeyup, then check
//                  the greeting follows (the event handling of the page).
//
// What it does: the stub parses the ids of index.html, runs app.js (Legacy) or the module script of
// index.html (Modern), and gives every <script src> that the page appends to the document the contents
// of the file in the folder, as a browser would
// (document.currentScript set while it runs, then onload; a missing file gives onerror). It waits
// until document.title is LOOMORA_PROBE_DONE or LOOMORA_PROBE_ERROR..., (probe mode) or until the
// greeting is displayed (no probe), then prints, one per line:
//   the probe record (the content of <pre id="probe">), a JSON line, when there is one
//   "title: <document.title>", "greeting: <text of the greeting element>"
// and exits 0 when everything is consistent (probe done, greeting equal to the record's message,
// "core" matching --path, "ui_variant" matching --variant), 1 otherwise (the reason on standard error),
// 2 on a usage error.
// Plain ES5 so that it also runs with the node of the emsdk.
"use strict";
var fs = require("fs");
var path = require("path");
var vm = require("vm");

var opts = { dist: null, variant: "legacy", streaming: "present",
             path: "js", query: null, typing: null, ua: "node page-harness", timeout: 30, verbose: false };
for (var ai = 2; ai < process.argv.length; ai++) {
  var a = process.argv[ai];
  if (a === "--verbose") opts.verbose = true;
  else if (a === "--dist") opts.dist = process.argv[++ai];
  else if (a === "--path") opts.path = process.argv[++ai];
  else if (a === "--variant") opts.variant = process.argv[++ai];
  else if (a === "--streaming") opts.streaming = process.argv[++ai];
  else if (a === "--query") opts.query = process.argv[++ai];
  else if (a === "--typing") opts.typing = process.argv[++ai];
  else if (a === "--ua") opts.ua = process.argv[++ai];
  else if (a === "--timeout") opts.timeout = Number(process.argv[++ai]);
  else { console.error("unknown argument " + a); process.exit(2); }
}
if (opts.path !== "js" && opts.path !== "wasm") { console.error("--path is js or wasm"); process.exit(2); }
if (opts.variant !== "legacy" && opts.variant !== "modern") { console.error("--variant is legacy or modern"); process.exit(2); }
if (opts.variant === "modern" && opts.path !== "wasm") { console.error("the Modern page has the wasm path only: use --path wasm"); process.exit(2); }
if (["present", "absent", "bad-mime"].indexOf(opts.streaming) < 0) { console.error("--streaming is present, absent or bad-mime"); process.exit(2); }
if (opts.dist === null) {
  opts.dist = process.env.LOOMORA_WEB_DIST || path.join(process.env.HOME || ".", "loomora-build", "web-" + opts.variant, "dist");
}
if (opts.query === "-") opts.query = fs.readFileSync(0, "utf8").replace(/[\r\n]+$/, "");

function problem(text) { console.error("FAIL " + text); process.exit(1); }
function log(text) { if (opts.verbose) console.error("info " + text); }

// ---- the DOM stub ----------------------------------------------------------------------------
function Node(type, tag) {
  this.nodeType = type; this.tagName = tag || ""; this.childNodes = []; this.parentNode = null;
  this.style = {}; this.nodeValue = null; this.attrs = {}; this.listeners = {};
}
Object.defineProperty(Node.prototype, "firstChild", { get: function () { return this.childNodes[0] || null; } });
Node.prototype.appendChild = function (child) {
  child.parentNode = this; this.childNodes.push(child);
  if (child.id && typeof byId !== "undefined" && !byId[child.id]) byId[child.id] = child;   // getElementById finds an element the page adds
  if (child.tagName === "script" && child.src && attached(this)) loadScript(child);
  return child;
};
Node.prototype.addEventListener = function (type, listener) {
  (this.listeners[type] = this.listeners[type] || []).push(listener);
};
Node.prototype.removeEventListener = function (type, listener) {
  var list = this.listeners[type] || [], i = list.indexOf(listener);
  if (i >= 0) list.splice(i, 1);
};
Node.prototype.dispatch = function (type) {
  var list = (this.listeners[type] || []).slice(), i;
  for (i = 0; i < list.length; i++) list[i].call(this, { type: type, target: this });
  return list.length;
};
Node.prototype.removeChild = function (child) {
  var i = this.childNodes.indexOf(child);
  if (i >= 0) this.childNodes.splice(i, 1);
  child.parentNode = null;
  return child;
};
Object.defineProperty(Node.prototype, "textContent", {
  get: function () { return textContent(this); },
  set: function (text) {
    var t = new Node(3, "");
    t.nodeValue = String(text); t.parentNode = this;
    this.childNodes = [t];
  }
});
function textContent(node) {
  if (node.nodeType === 3) return node.nodeValue;
  var s = "";
  for (var i = 0; i < node.childNodes.length; i++) s += textContent(node.childNodes[i]);
  return s;
}

var documentNode = new Node(9, "");
var html = new Node(1, "html"), head = new Node(1, "head"), body = new Node(1, "body");
documentNode.childNodes.push(html); html.parentNode = documentNode;
html.childNodes.push(head); head.parentNode = html;
html.childNodes.push(body); body.parentNode = html;
function attached(node) {
  while (node) { if (node === documentNode) return true; node = node.parentNode; }
  return false;
}

// A browser normalises the new lines of textarea.value: CRLF and a lone CR read back as LF (HTML,
// "API value"). The field of the page is a textarea, so the stub does the same; an <input
// type="text"> would strip the line feeds instead (that was the reason for the textarea).
function makeTextarea(node) {
  var stored = "";
  Object.defineProperty(node, "value", {
    get: function () { return stored; },
    set: function (v) { stored = String(v).replace(/\r\n?/g, "\n"); }
  });
}

var byId = {};
var pageHtml = fs.readFileSync(path.join(opts.dist, "index.html"), "utf8");
var re = /<(\w+)([^>]*?)>/g, m;
while ((m = re.exec(pageHtml)) !== null) {
  var idm = /\sid="([^"]+)"/.exec(m[2]);
  if (!idm) continue;
  var e = new Node(1, m[1].toLowerCase());
  var valm = /\svalue="([^"]*)"/.exec(m[2]);
  if (e.tagName === "textarea") {
    // The initial value of a textarea is its content (up to the closing tag), not an attribute.
    var endTag = pageHtml.indexOf("</textarea>", re.lastIndex);
    valm = [null, pageHtml.slice(re.lastIndex, endTag)];
    makeTextarea(e);
  }
  e.value = valm ? valm[1] : "";
  if (e.tagName === "textarea") {   // the default value is the content (no entity is decoded)
    var endTag = pageHtml.indexOf("</textarea>", re.lastIndex);
    e.value = endTag < 0 ? "" : pageHtml.substring(re.lastIndex, endTag);
  }
  e.id = idm[1];
  byId[e.id] = e;
  body.appendChild(e);
}
var titleMatch = /<title>([^<]*)<\/title>/.exec(pageHtml);

var doc = {
  title: titleMatch ? titleMatch[1] : "",
  currentScript: null,
  documentElement: html,
  head: head,
  body: body,
  getElementById: function (id) { return byId[id] || null; },
  getElementsByTagName: function (tag) { return tag === "head" ? [head] : tag === "body" ? [body] : []; },
  createElement: function (tag) { var n = new Node(1, tag.toLowerCase()); n.value = ""; return n; },
  createTextNode: function (text) { var n = new Node(3, ""); n.nodeValue = String(text); return n; }
};

// ---- the context -----------------------------------------------------------------------------
function consoleMethod(name) { return function (t) { log("console." + name + ": " + String(t).slice(0, 200)); }; }
var sandbox = { document: doc,
                console: { log: consoleMethod("log"), info: consoleMethod("info"), warn: consoleMethod("warn"),
                           error: consoleMethod("error"), debug: consoleMethod("debug") },
                setTimeout: setTimeout, clearTimeout: clearTimeout, setInterval: setInterval,
                clearInterval: clearInterval };
sandbox.window = sandbox; sandbox.self = sandbox;
sandbox.location = { search: opts.query === null ? "" : opts.query, href: "file:///" + opts.dist.replace(/^\//, "") + "/index.html" + (opts.query || "") };
sandbox.navigator = { userAgent: opts.ua };
if (opts.variant === "modern") {
  // What a current browser gives the Modern page: the encoders, the Response of fetch and
  // WebAssembly.instantiateStreaming, which needs a Response of type application/wasm.
  sandbox.TextEncoder = TextEncoder; sandbox.TextDecoder = TextDecoder;
  sandbox.performance = performance; sandbox.queueMicrotask = queueMicrotask;
  var wa = {};
  Object.getOwnPropertyNames(WebAssembly).forEach(function (k) {
    if (opts.streaming !== "absent" || (k !== "instantiateStreaming" && k !== "compileStreaming")) wa[k] = WebAssembly[k];
  });
  sandbox.WebAssembly = wa;
  sandbox.fetch = function (url) {
    var file = path.join(opts.dist, path.basename(String(url).split("?")[0]));
    var type = /\.wasm$/.test(file) && opts.streaming !== "bad-mime" ? "application/wasm" : "application/octet-stream";
    return fs.promises.readFile(file).then(function (data) {
      return new Response(data, { status: 200, headers: { "Content-Type": type } });
    }, function () { return new Response("not found", { status: 404 }); });
  };
} else if (opts.path === "wasm") {
  sandbox.WebAssembly = WebAssembly;
  // The glue of Emscripten 7.0.0 reads loomora.wasm with fetch in a web environment.
  sandbox.fetch = function (url) {
    var file = path.join(opts.dist, path.basename(String(url).split("?")[0]));
    return new Promise(function (resolve, reject) {
      fs.readFile(file, function (err, data) {
        if (err) { reject(err); return; }
        var ab = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
        resolve({ ok: true, status: 200, url: String(url), headers: { get: function () { return null; } },
                  arrayBuffer: function () { return Promise.resolve(ab); } });
      });
    });
  };
}
var ctx = vm.createContext(sandbox);
if (opts.path === "js") {
  // An engine that has no WebAssembly, no BigInt and no Promise: the page has to take the JS path.
  vm.runInContext("delete this.WebAssembly; delete this.BigInt; delete this.Promise;", ctx);
  if (vm.runInContext("typeof WebAssembly + typeof BigInt + typeof Promise", ctx) !== "undefinedundefinedundefined")
    problem("could not remove WebAssembly, BigInt and Promise from the context");
}

var scriptErrors = [];
function runScript(file, el) {
  var code;
  try { code = fs.readFileSync(path.join(opts.dist, file), "utf8"); }
  catch (err) { if (el && el.onerror) el.onerror(); return; }
  doc.currentScript = el || null;
  try { vm.runInContext(code, ctx, { filename: file }); }
  catch (err) { scriptErrors.push(file + ": " + (err && err.stack || err)); }
  doc.currentScript = null;
  if (el && el.onload) el.onload();
}
// The Modern page: the module script of index.html, run with the ES module support of vm
// (the imports of ./native.js are read from the folder).
function runModule(file) {
  var Module = vm.SourceTextModule, cache = {};
  if (typeof Module !== "function") { console.error("the Modern page needs node --experimental-vm-modules"); process.exit(2); }
  function load(name) {
    var bare = name.replace(/^\.\//, "");
    if (/[\/]|\.\./.test(bare)) throw new Error("module path not allowed: " + name);
    if (!cache[bare]) cache[bare] = new Module(fs.readFileSync(path.join(opts.dist, bare), "utf8"), { identifier: bare, context: ctx });
    return cache[bare];
  }
  var main = load(file);
  main.link(function (specifier) { return load(specifier); }).then(function () {
    return main.evaluate();
  }).then(null, function (err) { scriptErrors.push(file + ": " + (err && err.stack || err)); });
}

function loadScript(el) {
  var name = String(el.src);
  if (name.indexOf("/") >= 0 || name.indexOf("\\") >= 0 || name.indexOf("..") >= 0) { setImmediate(function () { if (el.onerror) el.onerror(); }); return; }
  log("page loads " + name);
  setImmediate(function () { runScript(name, el); });
}

// ---- run ---------------------------------------------------------------------------------------
var started = Date.now();
var probeMode = opts.query !== null && /(^|[?&])probe(=|&|$)/.test(opts.query);
if (opts.variant === "modern") {
  var moduleScript = /<script[^>]*\stype="module"[^>]*\ssrc="([^"]+)"/.exec(pageHtml);
  if (!moduleScript) problem("index.html has no <script type=\"module\" src=...>");
  runModule(moduleScript[1]);
} else {
  runScript("app.js", null);
  if (scriptErrors.length) problem("app.js threw: " + scriptErrors[0]);
}

var typed = false;
function state() {
  var greeting = byId.greeting ? textContent(byId.greeting) : "";
  var title = doc.title;
  var status = byId.status ? textContent(byId.status) : "";
  return { greeting: greeting, title: title, status: status };
}
var timer = setInterval(function () {
  var s = state(), err = /^LOOMORA_PROBE_ERROR/.test(s.title) || /^Error:/.test(s.status) || scriptErrors.length > 0;
  var done = probeMode ? (s.title === "LOOMORA_PROBE_DONE" || err) : (s.greeting !== "" || err);
  if (done && !err && !probeMode && opts.typing !== null && !typed) {
    typed = true;
    byId.name.value = opts.typing;
    if (opts.variant === "modern") byId.name.dispatch("input");
    else if (typeof byId.name.onkeyup === "function") byId.name.onkeyup();
    return;
  }
  if (done || (Date.now() - started) / 1000 > opts.timeout) { clearInterval(timer); finish(s, err, done); }
}, 10);

function finish(s, err, done) {
  var line = byId.probe ? textContent(byId.probe) : "";
  var bad = [];
  if (scriptErrors.length) bad.push("script error: " + scriptErrors[0]);
  if (!done) bad.push("timeout after " + opts.timeout + " s");
  if (err && !scriptErrors.length) bad.push(probeMode ? s.title : s.status);
  if (probeMode && !err && done) {
    var rec;
    try { rec = JSON.parse(line); } catch (e) { rec = null; bad.push("the probe element is not JSON: " + line.slice(0, 100)); }
    if (rec) {
      if (rec.message !== s.greeting) bad.push("greeting element " + JSON.stringify(s.greeting.slice(0, 60)) + " differs from the record's message");
      var want = opts.path === "wasm" ? "cpp-wasm" : "cpp-js";
      if (rec.core !== want) bad.push("core is " + rec.core + ", expected " + want);
      if (rec.ui_variant !== opts.variant) bad.push("ui_variant is " + rec.ui_variant + ", expected " + opts.variant);
      var banner = !!byId.eol && textContent(byId.eol) !== "";
      if (banner !== (rec.eol_warning_shown === true)) bad.push("eol_warning_shown " + rec.eol_warning_shown + " but the notice is " + (banner ? "shown" : "not shown"));
    }
  }
  if (!probeMode && !err && done) {
    var input = typed && opts.typing !== null ? opts.typing : "Coucou";
    if (s.greeting !== "Bonjour " + input) bad.push("greeting " + JSON.stringify(s.greeting) + ", expected " + JSON.stringify("Bonjour " + input));
  }
  if (line) console.log(line);
  console.log("eol notice: " + (byId.eol ? textContent(byId.eol) : "none"));
  console.log("title: " + s.title);
  console.log("greeting: " + s.greeting.slice(0, 200).replace(/[\r\n]/g, " "));
  if (bad.length) { console.error("FAIL " + bad.join("; ")); process.exit(1); }
  process.exit(0);
}
